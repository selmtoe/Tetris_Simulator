#!/usr/bin/env python3
"""Train KASANE-Base with grouped Cold Clear distillation rankings.

This is a warm start, not the terminal objective. The exported policy/value
network belongs to KASANE and is consumed by its independent beam search;
Cold Clear is not called for KASANE's own moves at inference time.
"""

from __future__ import annotations

import argparse
import json
import math
import random
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import torch
from torch import nn


@dataclass
class Group:
    features: np.ndarray
    teacher_scores: np.ndarray
    chosen: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", type=Path, nargs="+", required=True)
    parser.add_argument("--bootstrap", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--epochs", type=int, default=24)
    parser.add_argument("--batch-groups", type=int, default=64)
    parser.add_argument("--learning-rate", type=float, default=1.5e-3)
    parser.add_argument("--weight-decay", type=float, default=2e-5)
    parser.add_argument("--hidden", type=int, nargs=2, default=(64, 32))
    parser.add_argument("--seed", type=int, default=211021)
    parser.add_argument("--device", choices=("auto", "cpu", "cuda"), default="auto")
    return parser.parse_args()


def load_dataset(paths: list[Path]) -> tuple[dict, list[Group]]:
    metadata: dict | None = None
    groups: list[Group] = []
    for path in paths:
        with path.open("r", encoding="utf-8") as source:
            for line in source:
                item = json.loads(line)
                if item["type"] == "metadata":
                    if metadata is not None and metadata["feature_names"] != item["feature_names"]:
                        raise RuntimeError("dataset feature schemas differ")
                    metadata = item
                    continue
                features = np.asarray(item["features"], dtype=np.float32)
                teacher = np.asarray(item["teacher_scores"], dtype=np.float32)
                if len(features) < 2 or len(features) != len(teacher):
                    continue
                groups.append(Group(features, teacher, int(item["chosen"])))
    if metadata is None:
        raise RuntimeError("dataset metadata is missing")
    if not groups:
        raise RuntimeError("dataset contains no usable groups")
    return metadata, groups


class Ranker(nn.Module):
    def __init__(self, width: int, hidden: tuple[int, int]) -> None:
        super().__init__()
        self.linear = nn.Linear(width, 1)
        self.first = nn.Linear(width, hidden[0])
        self.second = nn.Linear(hidden[0], hidden[1])
        self.output = nn.Linear(hidden[1], 1)

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        residual = self.linear(value)
        hidden = torch.relu(self.first(value))
        hidden = torch.relu(self.second(hidden))
        return (residual + self.output(hidden)).squeeze(-1)


def statistics(groups: list[Group]) -> tuple[np.ndarray, np.ndarray]:
    values = np.concatenate([group.features for group in groups], axis=0).astype(np.float64)
    mean = values.mean(axis=0)
    scale = values.std(axis=0)
    scale[scale < 1e-4] = 1.0
    return mean.astype(np.float32), scale.astype(np.float32)


def initialize_from_bootstrap(
    model: Ranker, bootstrap: dict, mean: np.ndarray, scale: np.ndarray
) -> None:
    old_weight = np.asarray(bootstrap["linear_weights"], dtype=np.float32)
    old_scale = np.asarray(bootstrap["input_scale"], dtype=np.float32)
    old_mean = np.asarray(bootstrap["input_mean"], dtype=np.float32)
    transformed_weight = old_weight * scale / old_scale
    transformed_bias = float(bootstrap["linear_bias"]) + float(
        np.sum(old_weight * (mean - old_mean) / old_scale)
    )
    with torch.no_grad():
        model.linear.weight.copy_(torch.from_numpy(transformed_weight[None, :]))
        model.linear.bias.fill_(transformed_bias)
        nn.init.kaiming_uniform_(model.first.weight, a=math.sqrt(5))
        nn.init.zeros_(model.first.bias)
        nn.init.kaiming_uniform_(model.second.weight, a=math.sqrt(5))
        nn.init.zeros_(model.second.bias)
        nn.init.normal_(model.output.weight, std=0.01)
        nn.init.zeros_(model.output.bias)


def make_batch(
    groups: list[Group], indices: list[int], mean: np.ndarray, scale: np.ndarray, device: torch.device
) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor, torch.Tensor]:
    selected = [groups[index] for index in indices]
    maximum = max(len(group.features) for group in selected)
    width = selected[0].features.shape[1]
    features = np.zeros((len(selected), maximum, width), dtype=np.float32)
    teacher = np.full((len(selected), maximum), -1e9, dtype=np.float32)
    mask = np.zeros((len(selected), maximum), dtype=bool)
    chosen = np.empty(len(selected), dtype=np.int64)
    for row, group in enumerate(selected):
        count = len(group.features)
        features[row, :count] = (group.features - mean) / scale
        values = group.teacher_scores
        z = (values - values.mean()) / max(float(values.std()), 1.0)
        teacher[row, :count] = z
        mask[row, :count] = True
        chosen[row] = group.chosen
    return (
        torch.from_numpy(features).to(device),
        torch.from_numpy(teacher).to(device),
        torch.from_numpy(mask).to(device),
        torch.from_numpy(chosen).to(device),
    )


def group_loss(
    model: Ranker,
    features: torch.Tensor,
    teacher: torch.Tensor,
    mask: torch.Tensor,
    chosen: torch.Tensor,
) -> tuple[torch.Tensor, torch.Tensor]:
    logits = model(features)
    logits = logits.masked_fill(~mask, -1e9)
    hard = nn.functional.cross_entropy(logits, chosen)
    target = torch.softmax(teacher.masked_fill(~mask, -1e9) / 0.75, dim=1)
    soft = -(target * torch.log_softmax(logits, dim=1)).sum(dim=1).mean()
    loss = 0.90 * hard + 0.10 * soft
    accuracy = (logits.argmax(dim=1) == chosen).float().mean()
    return loss, accuracy


@torch.no_grad()
def evaluate(
    model: Ranker,
    groups: list[Group],
    indices: list[int],
    mean: np.ndarray,
    scale: np.ndarray,
    batch_size: int,
    device: torch.device,
) -> tuple[float, float]:
    model.eval()
    losses: list[float] = []
    accuracies: list[float] = []
    for begin in range(0, len(indices), batch_size):
        batch = make_batch(groups, indices[begin : begin + batch_size], mean, scale, device)
        loss, accuracy = group_loss(model, *batch)
        losses.append(float(loss))
        accuracies.append(float(accuracy))
    return float(np.mean(losses)), float(np.mean(accuracies))


def export_model(
    path: Path,
    metadata: dict,
    model: Ranker,
    mean: np.ndarray,
    scale: np.ndarray,
) -> None:
    state = model.to("cpu").state_dict()
    payload = {
        "schema": "kasane-base-model/v2-cell",
        "search_mode": "direct_policy",
        "feature_names": metadata["feature_names"],
        "input_mean": mean.tolist(),
        "input_scale": scale.tolist(),
        "linear_weights": state["linear.weight"][0].tolist(),
        "linear_bias": float(state["linear.bias"][0]),
        "hidden_layers": [
            {
                "weights": state["first.weight"].tolist(),
                "bias": state["first.bias"].tolist(),
            },
            {
                "weights": state["second.weight"].tolist(),
                "bias": state["second.bias"].tolist(),
            },
        ],
        "neural_output_weights": state["output.weight"][0].tolist(),
        "neural_output_bias": float(state["output.bias"][0]),
    }
    # The dedicated PC feature is forcibly neutral. Ordinary line-clear and
    # empty-board geometry still retain their natural value.
    pc = payload["feature_names"].index("perfect_clear")
    payload["linear_weights"][pc] = 0.0
    for layer in payload["hidden_layers"][:1]:
        for row in layer["weights"]:
            row[pc] = 0.0
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def main() -> None:
    args = parse_args()
    random.seed(args.seed)
    np.random.seed(args.seed)
    torch.manual_seed(args.seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(args.seed)
    device = torch.device(
        "cuda" if args.device == "auto" and torch.cuda.is_available() else
        "cpu" if args.device == "auto" else args.device
    )
    metadata, groups = load_dataset(args.dataset)
    bootstrap = json.loads(args.bootstrap.read_text(encoding="utf-8"))
    if bootstrap["feature_names"] != metadata["feature_names"]:
        raise RuntimeError("bootstrap and dataset feature schemas differ")

    indices = list(range(len(groups)))
    random.shuffle(indices)
    validation_count = max(1, len(indices) // 10)
    validation = indices[:validation_count]
    training = indices[validation_count:]
    mean, scale = statistics([groups[index] for index in training])
    model = Ranker(len(metadata["feature_names"]), tuple(args.hidden)).to(device)
    initialize_from_bootstrap(model, bootstrap, mean, scale)
    model.to(device)
    optimizer = torch.optim.AdamW(
        model.parameters(), lr=args.learning_rate, weight_decay=args.weight_decay
    )

    history: list[dict] = []
    best_accuracy = -1.0
    best_state: dict[str, torch.Tensor] | None = None
    for epoch in range(1, args.epochs + 1):
        random.shuffle(training)
        model.train()
        train_losses: list[float] = []
        train_accuracies: list[float] = []
        for begin in range(0, len(training), args.batch_groups):
            batch_indices = training[begin : begin + args.batch_groups]
            batch = make_batch(groups, batch_indices, mean, scale, device)
            optimizer.zero_grad(set_to_none=True)
            loss, accuracy = group_loss(model, *batch)
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), 5.0)
            optimizer.step()
            train_losses.append(float(loss.detach()))
            train_accuracies.append(float(accuracy.detach()))
        val_loss, val_accuracy = evaluate(
            model, groups, validation, mean, scale, args.batch_groups, device
        )
        row = {
            "epoch": epoch,
            "train_loss": float(np.mean(train_losses)),
            "train_top1": float(np.mean(train_accuracies)),
            "validation_loss": val_loss,
            "validation_top1": val_accuracy,
        }
        history.append(row)
        print(json.dumps(row))
        if val_accuracy > best_accuracy:
            best_accuracy = val_accuracy
            best_state = {name: value.detach().cpu().clone() for name, value in model.state_dict().items()}

    assert best_state is not None
    model.load_state_dict(best_state)
    export_model(args.output, metadata, model, mean, scale)
    report = {
        "schema": "kasane-base-training-report/v1",
        "dataset": [str(path) for path in args.dataset],
        "output": str(args.output),
        "device": str(device),
        "states": len(groups),
        "training_states": len(training),
        "validation_states": len(validation),
        "best_validation_top1": best_accuracy,
        "history": history,
    }
    report_path = args.report or args.output.with_suffix(".training.json")
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "history"}, indent=2))


if __name__ == "__main__":
    main()
