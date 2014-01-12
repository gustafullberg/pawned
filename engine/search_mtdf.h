#ifndef _SEARCH_MTDF_H
#define _SEARCH_MTDF_H

#include "state.h"
#include "search.h"

int SEARCH_mtdf(const chess_state_t *s, search_state_t *search_state, short depth, move_t *move, int guess);
int SEARCH_mtdf_iterative(const chess_state_t *s, search_state_t *search_state, move_t *move);

#endif
