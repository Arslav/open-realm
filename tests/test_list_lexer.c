#include "test.h"
#include "common/shared.h"
#include <ctype.h>

/* Compile both real list readers; capture only their bulk memory writes. */
static size_t lexer_clear_bytes;
static void *lexer_memset(void *dest, int value, size_t size) {
    lexer_clear_bytes += size; return memset(dest, value, size);
}
#define eat_token JLex_TestEatToken
#define parse_token JLex_TestParseToken
#define jlex_parse_token JLex_TestPublicToken
#define peek_token JLex_TestPeekToken
#define parse_segment JLex_TestSegment
#define parse_segment2 JLex_TestSegment2
#define parser_error JLex_TestError
#define find_in_array JLex_TestFind
#include "games/warcraft-3/jass/jlex.h"
#pragma push_macro("memset")
#undef memset
#define memset lexer_memset
#include "games/warcraft-3/jass/jlex.c"
#undef find_in_array
#undef parser_error
#undef parse_segment2
#undef parse_segment
#undef peek_token
#undef jlex_parse_token
#undef parse_token
#undef eat_token
#define STB_FDF_IMPLEMENTATION
#include "games/warcraft-3/common/stb_fdf.h"
#undef STB_FDF_IMPLEMENTATION
#pragma pop_macro("memset")

typedef cstring_t (*segmentReader_t)(wordExtractor_t *);
static segmentReader_t const readers[] = {JLex_TestSegment, parse_segment};

/* A nested decode owns its token buffer; truncation must still consume the complete source segment. */
TEST(list_lexer, caller_owned_segments_preserve_outer_token_and_bound_writes) {
    wordExtractor_t outer = { .buffer = "A001,A002", .delimiters = "" };
    wordExtractor_t inner = { .buffer = "\"A003\",A004", .delimiters = "" };
    struct { char text[5]; char guard; } buf = { .guard = 37 };
    cstring_t token = parse_segment(&outer);
    T_STREQ(token, "A001");
    T_STREQ(parse_segment_into(&inner, buf.text, sizeof(buf.text)), "A003");
    T_STREQ(parse_segment_into(&inner, buf.text, sizeof(buf.text)), "A004");
    T_STREQ(token, "A001"); T_EQ(buf.guard, 37);
    inner.buffer = "abcdefghijk,A005";
    T_STREQ(parse_segment_into(&inner, buf.text, sizeof(buf.text)), "abcd");
    T_STREQ(parse_segment_into(&inner, buf.text, sizeof(buf.text)), "A005");
    T_NULL(parse_segment_into(&inner, buf.text, sizeof(buf.text)));
    inner.buffer = "\"abcdefghijk\",A006";
    T_STREQ(parse_segment_into(&inner, buf.text, sizeof(buf.text)), "abcd");
    T_STREQ(parse_segment_into(&inner, buf.text, sizeof(buf.text)), "A006");
    T_EQ(buf.guard, 37); T_STREQ(parse_segment(&outer), "A002");
}

/* Short ability IDs must not clear kilobytes of scratch; a shorter successor still needs its own terminator. */
TEST(list_lexer, short_tokens_keep_semantics_without_bulk_scratch_clears) {
    char longer[513]; memset(longer, 'x', sizeof(longer) - 1); longer[sizeof(longer) - 1] = 0;
    FOR_LOOP(i, sizeof(readers) / sizeof(*readers)) {
        wordExtractor_t parser = {.buffer = longer, .delimiters = ""};
        T_EQ(strlen(readers[i](&parser)), 512);
        parser.buffer = " \tAInv,\"AOcl\",,tail";
        lexer_clear_bytes = 0;
        T_STREQ(readers[i](&parser), "AInv"); T_STREQ(readers[i](&parser), "AOcl");
        T_STREQ(readers[i](&parser), ""); T_STREQ(readers[i](&parser), "tail");
        T_NULL(readers[i](&parser)); T_EQ(lexer_clear_bytes, 0);
        parser.buffer = "\"unfinished"; T_STREQ(readers[i](&parser), "unfinished");
        T_NULL(readers[i](&parser));
        parser.buffer = "\"\""; T_STREQ(readers[i](&parser), ""); T_NULL(readers[i](&parser));
    }
}

/* Quoted overflow truncates the value but advances over the whole source before the next token. */
TEST(list_lexer, bounded_quoted_values_and_blank_tails_preserve_reader_contracts) {
    char text[32772]; memset(text, 'q', sizeof(text)); text[0] = '"';
    memcpy(text + sizeof(text) - 6, "\",X\0", 4);
    FOR_LOOP(i, sizeof(readers) / sizeof(*readers)) {
        wordExtractor_t parser = {.buffer = text, .delimiters = ""};
        T_EQ(strlen(readers[i](&parser)), i ? 1023 : 16383);
        T_STREQ(readers[i](&parser), "X"); T_NULL(readers[i](&parser));
        parser.buffer = "  \t";
        if (!i) T_STREQ(readers[i](&parser), "");
        T_NULL(readers[i](&parser));
    }
}

TEST(stress_list_lexer, ability_lists) {
    FOR_LOOP(reader, sizeof(readers) / sizeof(*readers)) {
        uint32_t tokens = 0; lexer_clear_bytes = 0;
        wordExtractor_t parser = {.delimiters = ""};
        T_BENCH(reader ? "FDF: 1000 four-ability lists" : "JASS: 1000 four-ability lists", 100, {
            FOR_LOOP(i, 1000) {
                parser.buffer = "AInv,AHbh,AOcl,Adef";
                while (readers[reader](&parser)) tokens++;
            }
        });
        T_EQ(tokens, 400000); printf("list scratch cleared: %zu bytes\n", lexer_clear_bytes);
    }
}
