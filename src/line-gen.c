/* line-gen.c
 * This file contains the implementation for line_generator
 *
 * Copyright 2022 abb128
 * Modified by Grillo del Mal (2023)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/mman.h>
#include <glib.h>
#include <april_api.h>

#include "line-gen.h"

#include <string.h>
#include <assert.h>

#include <obs-frontend-api.h>

void token_capitalizer_init(struct token_capitalizer *tc) {
    tc->is_english = true;
    tc->previous_was_period = true;
    tc->finished_at_period = false;
    tc->force_next_cap = false;
}

bool token_capitalizer_next(struct token_capitalizer *tc, const char *token, int flags, const char *subsequent_token, int subsequent_flags) {
    if((flags & APRIL_TOKEN_FLAG_SENTENCE_END_BIT) != 0){
        tc->previous_was_period = true;
        return false;
    }

    if(tc->force_next_cap){
        tc->force_next_cap = false;
        return true;
    }

    if((tc->previous_was_period) && (flags & APRIL_TOKEN_FLAG_WORD_BOUNDARY_BIT)){
        // If bare space token, capitalize the subsequent token since space can't
        // be capitalized.
        if((token[0] == ' ') && (token[1] == 0)){
            tc->force_next_cap = true;
        }

        tc->previous_was_period = false;
        return true;
    }

    // English-specific behavior: capitalize 'I'
    // TODO: A better way of capitalizing I and names and places
    if(tc->is_english) {
        if((token[0] == ' ') && (token[1] == 'I') && (token[2] == 0)){
            if(subsequent_token != NULL){
                if (((subsequent_flags & (APRIL_TOKEN_FLAG_WORD_BOUNDARY_BIT | APRIL_TOKEN_FLAG_SENTENCE_END_BIT)) != 0) || (subsequent_token[0] == '\'')){
                    return true;
                }
            }else{
                return true;
            }
        }
    }

    return false;
}

void token_capitalizer_finish(struct token_capitalizer *tc){
    tc->finished_at_period = tc->previous_was_period;
    tc->previous_was_period = false;
    tc->force_next_cap = false;
}

void token_capitalizer_rewind(struct token_capitalizer *tc){
    tc->previous_was_period = tc->finished_at_period;
    tc->force_next_cap = false;
}

#define REL_LINE_IDX(HEAD, IDX) (4*AC_LINE_COUNT + (HEAD) + (IDX)) % AC_LINE_COUNT

void line_generator_init(struct line_generator *lg) {
    for(int i=0; i<AC_LINE_COUNT; i++){
        lg->active_start_of_lines[i] = -1;

        lg->lines[i].start_head = 0;
        lg->lines[i].start_len = 0;
    }

    lg->current_line = 0;
    lg->active_start_of_lines[0] = 0;
    lg->max_text_width = 50;

    token_capitalizer_init(&lg->tcap);
}

void line_generator_set_label(struct line_generator *lg, struct tp_source *text_src) {
    lg->text_src = text_src;
}

#define MAX_TOKEN_SCRATCH 72
void line_generator_update(struct line_generator *lg, size_t num_tokens, const AprilToken *tokens) {
    // Add capitalization information
    static bool should_capitalize[1024];

    token_capitalizer_rewind(&lg->tcap);
    for(size_t i=0; i<num_tokens; i++){
        if((i+1) < num_tokens) {
            should_capitalize[i] = token_capitalizer_next(&lg->tcap, tokens[i].token, tokens[i].flags, tokens[i+1].token, tokens[i+1].flags);
        }else{
            should_capitalize[i] = token_capitalizer_next(&lg->tcap, tokens[i].token, tokens[i].flags, NULL, 0);
        }
    }

    bool use_lowercase = true;//!g_settings_get_boolean(settings, "text-uppercase");
    char token_scratch[MAX_TOKEN_SCRATCH] = { 0 };

    for(size_t i=0; i<AC_LINE_COUNT; i++){
        if(lg->active_start_of_lines[i] == -1) continue;
        size_t start_of_line = lg->active_start_of_lines[i];

        struct line *curr = &lg->lines[i];

        // reset for writing
        curr->text[curr->start_head] = '\0';
        curr->head = curr->start_head;
        curr->len = curr->start_len;

        if(num_tokens == 0) continue;

        if(start_of_line >= num_tokens) {
            if(i == lg->current_line) {
                // oops... turns out our text isn't long enough for the new line
                // backtrack to the previous line
                lg->active_start_of_lines[lg->current_line] = -1;
                lg->current_line = REL_LINE_IDX(lg->current_line, -1);
                return line_generator_update(lg, num_tokens, tokens);
            } else {
                continue;
            }
        }


        ssize_t end = lg->active_start_of_lines[REL_LINE_IDX(i, 1)];
        if((end == -1) || (i == lg->current_line)) end = num_tokens;

        // print line
        for(size_t j=start_of_line; j<((size_t)end);) {
            size_t skipahead = 1;
            const char *token = tokens[j].token;

            bool should_be_capitalized = should_capitalize[j];

            if(use_lowercase){
                char *out = token_scratch;
                const char *p = tokens[j].token;
                gunichar c;
                while (*p) {
                    c = g_utf8_get_char_validated(p, -1);
                    if(c == ((gunichar)-2)) {
                        printf("gunichar -2 \n");
                        break;
                    }else if(c == ((gunichar)-1)) {
                        printf("gunichar -1 \n");
                        break;
                    }

                    c = g_unichar_tolower(c);

                    if(should_be_capitalized){
                        gunichar c1 = g_unichar_toupper(c);
                        if(c != c1){
                            c = c1;
                            should_be_capitalized = false;
                        }
                    }

                    out += g_unichar_to_utf8(c, out);
                    if((out + 6) >= (token_scratch + MAX_TOKEN_SCRATCH)){
                        printf("Unicode too big for token scratch!\n");
                        break;
                    }

                    p = g_utf8_next_char(p);
                }

                *out = '\0';

                token = token_scratch;
            }

            // skip if line is too long to safely write
            int must_break = (curr->head > (AC_LINE_MAX - 256));
            if(must_break){
                blog(LOG_WARNING, "[catpion] Must linebreak, but not active line. Leaving incomplete line...");
                break;
            }

            // break line if too long
            if(i == lg->current_line){
                curr->len += strlen(token);
                if(curr->len >= lg->max_text_width) {
                    size_t tgt_brk = j;
                    // find previous word boundary
                    while((!(tokens[tgt_brk].flags & APRIL_TOKEN_FLAG_WORD_BOUNDARY_BIT)) && (tgt_brk > start_of_line)) tgt_brk--;

                    // if we backtracked all the way to the start of line, just give up and break here
                    // unless this line has starting text
                    if((tgt_brk == start_of_line) && (curr->start_head == 0)) tgt_brk = j;

                    // line break
                    lg->current_line = REL_LINE_IDX(lg->current_line, 1);
                    lg->active_start_of_lines[lg->current_line] = tgt_brk;
                    lg->lines[lg->current_line].start_head = 0;
                    lg->lines[lg->current_line].start_len = 0;
                    return line_generator_update(lg, num_tokens, tokens);
                }
            }

            // write the actual line
            curr->head += sprintf(&curr->text[curr->head], "%s", token);
            
            assert(curr->head < AC_LINE_MAX);

            j += skipahead;
        }
    }
}

void line_generator_finalize(struct line_generator *lg) {
    // reset active
    for(size_t i=0; i<AC_LINE_COUNT; i++) lg->active_start_of_lines[i] = -1;

    // freeze the current line thus far
    lg->lines[lg->current_line].start_head = lg->lines[lg->current_line].head;
    lg->lines[lg->current_line].start_len = lg->lines[lg->current_line].len;

    // set new line to start at 0
    lg->active_start_of_lines[lg->current_line] = 0;
}

void line_generator_break(struct line_generator *lg) {
    // insert new line
    lg->current_line = REL_LINE_IDX(lg->current_line, 1);

    // reset active
    for(int i=0; i<AC_LINE_COUNT; i++) lg->active_start_of_lines[i] = -1;

    // set new line to start at 0
    lg->active_start_of_lines[lg->current_line] = 0;

    // clear new line
    lg->lines[lg->current_line].text[0] = '\0';
    lg->lines[lg->current_line].head = 0;
    lg->lines[lg->current_line].len = 0;
    lg->lines[lg->current_line].start_head = 0;
    lg->lines[lg->current_line].start_len = 0;
}

void line_generator_set_text(struct line_generator *lg) {
    static char last_sent[AC_LINE_MAX+100];
    char *head = &lg->output[0];
    *head = '\0';

    for(int i=AC_LINE_COUNT-1; i>=0; i--) {
        struct line *curr = &lg->lines[REL_LINE_IDX(lg->current_line, -i)];
        head += sprintf(head, "%s", curr->text);

        if(i == AC_LINE_COUNT-1){
            if(lg->to_stream){
                if(strcmp(last_sent, head) != 0){
                    blog(LOG_INFO, "[catpion] s: %s", head);
                    obs_output_t *output = NULL;
                    output = obs_frontend_get_streaming_output();
                    if (output) {
                        obs_output_output_caption_text2(output, head, 2.0);
                        obs_output_release(output);
                    }
                }
                strncpy(last_sent, head, AC_LINE_MAX+100);
            }
        }

        if(i != 0) head += sprintf(head, "\n");
        blog(LOG_DEBUG, "[catpion] %d/%d: %s", i, AC_LINE_COUNT-1,curr->text);
    }

    if(lg->text_src) tp_edit_text(lg->text_src, lg->output);
}