#include <stdlib.h>
#include <stdio.h>
#include "engine.h"
#include "state.h"
#include "ttable.h"
#include "history.h"
#include "openingbook.h"
#include "search.h"
#include "san.h"
#include "defines.h"

struct engine_state {
    chess_state_t   *chess_state;
    ttable_t        *ttable;
    history_t       *history;
    openingbook_t   *obook;
};

static void ENGINE_init()
{
    static int first_run = 1;
    if(first_run) {
        bitboard_init();
        first_run = 0;
    }
}
void ENGINE_create(engine_state_t **state)
{
    ENGINE_init();
    *state = malloc(sizeof(engine_state_t));
    (*state)->chess_state = malloc(sizeof(chess_state_t));
    (*state)->ttable = TTABLE_create(22);
    (*state)->history = HISTORY_create();
    (*state)->obook = OPENINGBOOK_create("openingbook.dat");
    ENGINE_reset(*state);
}

void ENGINE_destroy(engine_state_t *state)
{
    TTABLE_destroy(state->ttable);
    HISTORY_destroy(state->history);
    OPENINGBOOK_destroy(state->obook);
    free(state->chess_state);
    free(state);
}

void ENGINE_reset(engine_state_t *state)
{
    STATE_reset(state->chess_state);
    HISTORY_reset(state->history);
    OPENINGBOOK_reset(state->obook);
}

int ENGINE_apply_move(engine_state_t *state, int pos_from, int pos_to, int promotion_type)
{
    int num_moves;
    int i;
    chess_state_t temporary_state;
    move_t moves[256];
    
    /* Generate all possible moves */
    num_moves = STATE_generate_moves(state->chess_state, moves);
    
    /* Loop through all generated moves to find the right one */
    for(i = 0; i < num_moves; i++) {
        int move = moves[i];
        if(MOVE_GET_POS_FROM(move) != pos_from) continue;
        if(MOVE_GET_POS_TO(move) != pos_to) continue;
        if(MOVE_PROMOTION_TYPE(move) != promotion_type) continue;
        
        /* Pseudo legal move found: Apply to state */
        temporary_state = *state->chess_state;
        STATE_apply_move(&temporary_state, move);
        
        /* Check if the move is legal */
        if(SEARCH_is_check(&temporary_state, state->chess_state->player)) {
            /* Not legal */
            break;
        }
        
        /* Legal */
        *state->chess_state = temporary_state;
        HISTORY_push(state->history, state->chess_state->hash);
        OPENINGBOOK_apply_move(state->obook, move);
        return ENGINE_result(state);
    }
    
    /* No valid move found: Illegal move */
    return ENGINE_RESULT_ILLEGAL_MOVE;
}

int ENGINE_apply_move_san(engine_state_t *state, const char *san)
{
    move_t move;
    chess_state_t temporary_state;
    move = SAN_parse_move(state->chess_state, san);
    
    if(move) {
        /* Pseudo legal move found: Apply to state */
        temporary_state = *state->chess_state;
        STATE_apply_move(&temporary_state, move);
        
        /* Check if the move is legal */
        if(!SEARCH_is_check(&temporary_state, state->chess_state->player)) {
            /* Legal */
            *state->chess_state = temporary_state;
            HISTORY_push(state->history, state->chess_state->hash);
            OPENINGBOOK_apply_move(state->obook, move);
            return ENGINE_result(state);
        }
    }

    /* No valid move found: Illegal move */
    return ENGINE_RESULT_ILLEGAL_MOVE;
}

void ENGINE_think(engine_state_t *state, int moves_left_in_period, int time_left_ms, int time_incremental_ms, int *pos_from, int *pos_to, int *promotion_type, int max_depth)
{
    int move;
    int special;
    int score;
    int time_for_move_ms;
    
    if(moves_left_in_period) {
        time_for_move_ms = time_left_ms / moves_left_in_period;
        if(time_for_move_ms > time_left_ms - 100) {
            time_for_move_ms = time_left_ms - 100;
        }
    } else {
        time_for_move_ms = time_left_ms * 2 / 100;
    }

#ifndef DISABLE_OPENING_BOOK
    /* Look for a move in the opening book */
    move = OPENINGBOOK_get_move(state->obook, state->chess_state);
    if(!move)
#endif
    {
        /* No move in the opening book. Search! */
        move = SEARCH_perform_search(state->chess_state, state->ttable, state->history, time_for_move_ms, max_depth, &score);
    }

    *pos_from = MOVE_GET_POS_FROM(move);
    *pos_to = MOVE_GET_POS_TO(move);
    special = MOVE_GET_SPECIAL_FLAGS(move);
    
    switch(special)
    {
        case MOVE_KNIGHT_PROMOTION:
        case MOVE_KNIGHT_PROMOTION_CAPTURE:
        *promotion_type = ENGINE_PROMOTION_KNIGHT;
        break;
        
        case MOVE_BISHOP_PROMOTION:
        case MOVE_BISHOP_PROMOTION_CAPTURE:
        *promotion_type = ENGINE_PROMOTION_BISHOP;
        break;
        
        case MOVE_ROOK_PROMOTION:
        case MOVE_ROOK_PROMOTION_CAPTURE:
        *promotion_type = ENGINE_PROMOTION_ROOK;
        break;
        
        case MOVE_QUEEN_PROMOTION:
        case MOVE_QUEEN_PROMOTION_CAPTURE:
        *promotion_type = ENGINE_PROMOTION_QUEEN;
        break;
        
        default:
        *promotion_type = ENGINE_PROMOTION_NONE;
        break;
    }
}

int ENGINE_result(engine_state_t *state)
{
    if(SEARCH_is_mate(state->chess_state)) {
        if(SEARCH_is_check(state->chess_state, state->chess_state->player)) {
            if(state->chess_state->player == WHITE) {
                return ENGINE_RESULT_BLACK_MATES;
            } else {
                return ENGINE_RESULT_WHITE_MATES;
            }
        } else {
            return ENGINE_RESULT_STALE_MATE;
        }
    }
    
    return ENGINE_RESULT_NONE;
}
