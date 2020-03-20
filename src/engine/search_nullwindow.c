#include "search_nullwindow.h"
#include "search.h"
#include "eval.h"
#include "moveorder.h"
#include "clock.h"
#include "see.h"

static inline short SEARCH_transpositiontable_retrieve(const hashtable_t *hashtable, const bitboard_t hash, const unsigned char depth, short beta, move_t *best_move, int *cutoff);
static inline void SEARCH_transpositiontable_store(hashtable_t *hashtable, const bitboard_t hash, const unsigned char depth, const short best_score, move_t best_move, const short beta);

/* Alpha-Beta search with Nega Max and null-window */
short SEARCH_nullwindow(const chess_state_t *state, search_state_t *search_state, unsigned char depth, unsigned char ply, move_t *move, short beta)
{
    int num_moves;
    int num_legal_moves = 0;
    int i;
    short score;
    short best_score = SEARCH_MIN_RESULT(depth);
    int skip_move_generation = 0;
    move_t next_move;
    chess_state_t next_state;
    move_t moves[256];
    short ttable_score;
    int cutoff = 0;
    int do_futility_pruning = 0;

    *move = 0;

    /* Check if time is up */
    search_state->next_clock_check--;
    if(search_state->next_clock_check <= 0) {
        search_state->next_clock_check = SEARCH_ITERATIONS_BETWEEN_CLOCK_CHECK;
        if(CLOCK_time_passed(search_state->start_time_ms) >= search_state->time_for_move_ms) {
            search_state->abort_search = 1;
        }
    }
    if(search_state->abort_search) {
        return 0;
    }

    if(ply > MAX_SEARCH_DEPTH) ply = MAX_SEARCH_DEPTH;

    /* We will query the transition table soon, time to prefetch */
    HASHTABLE_transition_prefetch(search_state->hashtable, state->hash);

    /* Is playing side in check? */
    bitboard_t block_check, pinners, pinned;
    int num_checkers = STATE_checkers_and_pinners(state, &block_check, &pinners, &pinned);

    /* Check extension */
    if(num_checkers) {
        depth += 1;
    }
    /* Quiescence search */
    if(depth == 0) {
        return SEARCH_nullwindow_quiescence(state, search_state, beta);
    }

    /* Query the transposition table */
    ttable_score = SEARCH_transpositiontable_retrieve(search_state->hashtable, state->hash, depth, beta, move, &cutoff);
    if(cutoff) {
        if(*move || state->last_move) {
            return ttable_score;
        }
    }

    /* Null move pruning */
    if(depth > 4 && state->last_move && !num_checkers && !STATE_risk_zugzwang(state)) {
        unsigned char R_plus_1 = ((depth > 5) ? 4 : 3);
        next_state = *state;
        STATE_apply_move(&next_state, 0);
        score = -SEARCH_nullwindow(&next_state, search_state, depth-R_plus_1, ply+1, &next_move, -beta+1);
        if(score >= beta) {
            best_score = beta;
            skip_move_generation = 1;
        }
    }

    if(!skip_move_generation) {
        /* Generate and rate moves */
        num_moves = STATE_generate_legal_moves(state, num_checkers, block_check, pinners, pinned, moves);
        MOVEORDER_rate_moves(state, moves, num_moves, *move, search_state->killer_move[ply], search_state->history_heuristic[state->player]);

        /* Check if node is eligible for futility pruning */
        if(depth <= 3 && !num_checkers) {
            if(beta > EVAL_evaluate_board(state) + 25) {
                do_futility_pruning = 1;
            }
        }

        /* Iterate over all pseudo legal moves */
        for(i = 0; i < num_moves; i++) {
            /* Pick move with the highest score */
            MOVEORDER_best_move_first(&moves[i], num_moves - i);

            /* Apply move */
            next_state = *state;
            STATE_apply_move(&next_state, moves[i]);

            /* Futility pruning */
            if(do_futility_pruning) {
                if(num_legal_moves > 1 && !MOVE_IS_CAPTURE_OR_PROMOTION(moves[i]) && !SEARCH_is_check(&next_state, next_state.player)) {
                    continue;
                }
            }

            num_legal_moves++;

            HISTORY_push(search_state->history, next_state.hash);
            if(HISTORY_is_repetition(search_state->history, next_state.halfmove_clock) || EVAL_insufficient_material(&next_state)) {
                /* Repetition detected */
                score = 0;
            } else {
                /* Late move reduction */
                if( num_legal_moves > 4                     && /* Four moves have been searched at full depth   */
                    depth >= 3                              && /* No LMR in the last plies                      */
                    !MOVE_IS_CAPTURE_OR_PROMOTION(moves[i]))   /* No LMR if capture / promotion                 */
                {
                    /* Search at reduced depth */
                    unsigned char R_plus_1 = (num_legal_moves > 15 && depth > 3) ? 3 : 2;
                    score = -SEARCH_nullwindow(&next_state, search_state, depth-R_plus_1, ply+1, &next_move, -beta+1);
                    if(score >= beta) {
                        /* If reduced yields interesting results, do a full search */
                        score = -SEARCH_nullwindow(&next_state, search_state, depth-1, ply+1, &next_move, -beta+1);
                    }
                } else {
                    /* Normal search */
                    score = -SEARCH_nullwindow(&next_state, search_state, depth-1, ply+1, &next_move, -beta+1);
                }
            }
            HISTORY_pop(search_state->history);

            /* Check if score improved by this move */
            if(score > best_score) {
                best_score = score;
                *move = moves[i];

                /* Beta-cuttoff */
                if(best_score >= beta) {
                    if(!MOVE_IS_CAPTURE_OR_PROMOTION(*move)) {
                        /* Killer move */
                        if(*move != search_state->killer_move[ply][0]) {
                            search_state->killer_move[ply][1] = search_state->killer_move[ply][0];
                            search_state->killer_move[ply][0] = *move;
                        }
                        /* History heuristic */
                        search_state->history_heuristic[state->player][MOVE_GET_POS_FROM(*move)][MOVE_GET_POS_TO(*move)] += depth*depth;
                    }
                    break;
                }
            }
        }

        /* Detect checkmate and stalemate */
        if(num_legal_moves == 0) {
            if(num_checkers) {
                /* Checkmate (worst case) */
            } else {
                /* Stalemate */
                best_score = 0;
            }
        }
    }

    /* Store the result in the transposition table */
    if(!search_state->abort_search) {
        SEARCH_transpositiontable_store(search_state->hashtable, state->hash, depth, best_score, *move, beta);
    }

    /* Return score */
    return best_score;
}

/* Alpha-Beta quiescence search with Nega Max and null-window */
short SEARCH_nullwindow_quiescence(const chess_state_t *state, search_state_t *search_state, short beta)
{
    int num_moves;
    int i;
    short score;
    short best_score;
    chess_state_t next_state;
    move_t moves[256];

    /* Is playing side in check? */
    bitboard_t block_check, pinners, pinned;
    int num_checkers = STATE_checkers_and_pinners(state, &block_check, &pinners, &pinned);

    if(num_checkers) best_score = SEARCH_MIN_RESULT(0);
    else {
        /* Stand-pat */
        best_score = EVAL_evaluate_board(state);
        search_state->num_nodes_searched++;
        if(best_score >= beta) {
            return best_score;
        }
    }

    /* Generate and rate moves (captures and promotions only) */
    if(num_checkers) {
        num_moves = STATE_generate_legal_moves(state, num_checkers, block_check, pinners, pinned, moves);
    } else {
        num_moves = STATE_generate_legal_moves_quiescence(state, num_checkers, block_check, pinners, pinned, moves);
    }
    MOVEORDER_rate_moves_quiescence(state, moves, num_moves);

    for(i = 0; i < num_moves; i++) {
        /* Pick move with the highest score */
        MOVEORDER_best_move_first(&moves[i], num_moves - i);

        /* Prune all captures with SEE < 0 */
        if(!MOVE_IS_PROMOTION(moves[i]) && !num_checkers) {
            if(SSE_capture_less_valuable(moves[i]) && see(state, moves[i]) < 0) {
                continue;
            }
        }

        next_state = *state;
        STATE_apply_move(&next_state, moves[i]);

        score = -SEARCH_nullwindow_quiescence(&next_state, search_state, -beta+1);
        if(score > best_score) {
            best_score = score;
            if(best_score >= beta) {
                /* Beta-cuttoff */
                break;
            }
        }
    }

    return best_score;
}

static inline short SEARCH_transpositiontable_retrieve(const hashtable_t *hashtable, const bitboard_t hash, const unsigned char depth, short beta, move_t *best_move, int *cutoff)
{
    transposition_entry_t *ttentry = HASHTABLE_transition_retrieve(hashtable, hash);
    if(ttentry) {
        *best_move = ttentry->best_move;

        if(ttentry->depth >= depth) {
            short score = ttentry->score;
            if(ttentry->type == TTABLE_TYPE_UPPER_BOUND) {
                if(score < beta) {
                    short min = SEARCH_MIN_RESULT(0);
                    *cutoff = 1;
                    return (score <= min) ? score + ttentry->depth - depth : score;
                }
            } else { /* TTABLE_TYPE_LOWER_BOUND */
                if(score >= beta) {
                    short max = SEARCH_MAX_RESULT(0);
                    *cutoff = 1;
                    return (score >= max) ? score - ttentry->depth + depth : score;
                }
            }
        }

        *cutoff = 0;
    }

    return 0;
}

static inline void SEARCH_transpositiontable_store(hashtable_t *hashtable, const bitboard_t hash, const unsigned char depth, const short best_score, move_t best_move, const short beta)
{
    if(best_score < beta) {
        HASHTABLE_transition_store(hashtable, hash, depth, TTABLE_TYPE_UPPER_BOUND, best_score, best_move);
    } else {
        HASHTABLE_transition_store(hashtable, hash, depth, TTABLE_TYPE_LOWER_BOUND, best_score, best_move);
    }
}
