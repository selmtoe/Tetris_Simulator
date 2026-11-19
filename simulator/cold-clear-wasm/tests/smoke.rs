use simulator_cold_clear_wasm::{cc_add_next_piece, cc_commit, cc_create, cc_destroy, cc_suggest, cc_think, CcMove};

#[test]
fn reference_core_can_search_and_commit() {
    let board = [0u8; 400];
    let next = *b"IOLJSZTI";
    let weights = br#"{"back_to_back":52,"height":-39}"#;
    let bot = unsafe {
        cc_create(
            board.as_ptr(),
            b'T',
            next.as_ptr(),
            next.len() as u32,
            0,
            1,
            0,
            -1,
            2000,
            weights.as_ptr(),
            weights.len() as u32,
        )
    };
    assert!(!bot.is_null());
    let nodes = unsafe { cc_think(bot, 32) };
    assert!(nodes > 1, "reference DAG did not expand: {}", nodes);

    let mut result = CcMove::default();
    assert_eq!(unsafe { cc_suggest(bot, 0, &mut result) }, 1);
    assert!(b"IOTLJSZ".contains(&result.piece));
    assert!(result.x >= 0 && result.x < 10);
    assert!(result.y >= 0 && result.y < 40);
    assert!(unsafe { cc_commit(bot) } != 0);
    assert!(unsafe { cc_add_next_piece(bot, b'Z') } != 0);
    unsafe { cc_destroy(bot) };
}
