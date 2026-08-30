use anyhow::Result;
use clap::Parser;
use kasane::agent::StackRenModel;
use std::path::PathBuf;

#[derive(Debug, Parser)]
#[command(about = "Export the deterministic Stack-REN v3 bootstrap MLP")]
struct Cli {
    #[arg(long, default_value = "models/stack-ren-model-v3.json")]
    output: PathBuf,
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    StackRenModel::bootstrap().save(&cli.output)?;
    println!("wrote {}", cli.output.display());
    Ok(())
}
