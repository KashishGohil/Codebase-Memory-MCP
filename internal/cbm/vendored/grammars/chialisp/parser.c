#include "tree_sitter/parser.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#define LANGUAGE_VERSION 14
#define STATE_COUNT 15
#define LARGE_STATE_COUNT 12
#define SYMBOL_COUNT 13
#define ALIAS_COUNT 0
#define TOKEN_COUNT 9
#define EXTERNAL_TOKEN_COUNT 0
#define FIELD_COUNT 0
#define MAX_ALIAS_SEQUENCE_LENGTH 5
#define PRODUCTION_ID_COUNT 1

enum ts_symbol_identifiers {
  anon_sym_LPAREN = 1,
  anon_sym_RPAREN = 2,
  sym_dot = 3,
  sym_hex = 4,
  sym_number = 5,
  sym_symbol = 6,
  sym_string = 7,
  sym_comment = 8,
  sym_source_file = 9,
  sym__sexp = 10,
  sym_list = 11,
  aux_sym_source_file_repeat1 = 12,
};

static const char * const ts_symbol_names[] = {
  [ts_builtin_sym_end] = "end",
  [anon_sym_LPAREN] = "(",
  [anon_sym_RPAREN] = ")",
  [sym_dot] = "dot",
  [sym_hex] = "hex",
  [sym_number] = "number",
  [sym_symbol] = "symbol",
  [sym_string] = "string",
  [sym_comment] = "comment",
  [sym_source_file] = "source_file",
  [sym__sexp] = "_sexp",
  [sym_list] = "list",
  [aux_sym_source_file_repeat1] = "source_file_repeat1",
};

static const TSSymbol ts_symbol_map[] = {
  [ts_builtin_sym_end] = ts_builtin_sym_end,
  [anon_sym_LPAREN] = anon_sym_LPAREN,
  [anon_sym_RPAREN] = anon_sym_RPAREN,
  [sym_dot] = sym_dot,
  [sym_hex] = sym_hex,
  [sym_number] = sym_number,
  [sym_symbol] = sym_symbol,
  [sym_string] = sym_string,
  [sym_comment] = sym_comment,
  [sym_source_file] = sym_source_file,
  [sym__sexp] = sym__sexp,
  [sym_list] = sym_list,
  [aux_sym_source_file_repeat1] = aux_sym_source_file_repeat1,
};

static const TSSymbolMetadata ts_symbol_metadata[] = {
  [ts_builtin_sym_end] = {
    .visible = false,
    .named = true,
  },
  [anon_sym_LPAREN] = {
    .visible = true,
    .named = false,
  },
  [anon_sym_RPAREN] = {
    .visible = true,
    .named = false,
  },
  [sym_dot] = {
    .visible = true,
    .named = true,
  },
  [sym_hex] = {
    .visible = true,
    .named = true,
  },
  [sym_number] = {
    .visible = true,
    .named = true,
  },
  [sym_symbol] = {
    .visible = true,
    .named = true,
  },
  [sym_string] = {
    .visible = true,
    .named = true,
  },
  [sym_comment] = {
    .visible = true,
    .named = true,
  },
  [sym_source_file] = {
    .visible = true,
    .named = true,
  },
  [sym__sexp] = {
    .visible = false,
    .named = true,
  },
  [sym_list] = {
    .visible = true,
    .named = true,
  },
  [aux_sym_source_file_repeat1] = {
    .visible = false,
    .named = false,
  },
};

static const TSSymbol ts_alias_sequences[PRODUCTION_ID_COUNT][MAX_ALIAS_SEQUENCE_LENGTH] = {
  [0] = {0},
};

static const uint16_t ts_non_terminal_alias_map[] = {
  0,
};

static const TSStateId ts_primary_state_ids[STATE_COUNT] = {
  [0] = 0,
  [1] = 1,
  [2] = 2,
  [3] = 3,
  [4] = 4,
  [5] = 5,
  [6] = 6,
  [7] = 7,
  [8] = 8,
  [9] = 9,
  [10] = 10,
  [11] = 11,
  [12] = 12,
  [13] = 13,
  [14] = 14,
};

static bool ts_lex(TSLexer *lexer, TSStateId state) {
  START_LEXER();
  eof = lexer->eof(lexer);
  switch (state) {
    case 0:
      if (eof) ADVANCE(7);
      ADVANCE_MAP(
        '"', 1,
        '\'', 2,
        '(', 8,
        ')', 9,
        '-', 14,
        '.', 10,
        '0', 12,
        ';', 18,
      );
      if (('\t' <= lookahead && lookahead <= '\r') ||
          lookahead == ' ') SKIP(0);
      if (('1' <= lookahead && lookahead <= '9')) ADVANCE(13);
      if (lookahead != 0) ADVANCE(16);
      END_STATE();
    case 1:
      if (lookahead == '"') ADVANCE(17);
      if (lookahead == '\\') ADVANCE(4);
      if (lookahead != 0) ADVANCE(1);
      END_STATE();
    case 2:
      if (lookahead == '\'') ADVANCE(17);
      if (lookahead == '\\') ADVANCE(5);
      if (lookahead != 0) ADVANCE(2);
      END_STATE();
    case 3:
      if (lookahead == '.' ||
          ('0' <= lookahead && lookahead <= '9')) ADVANCE(3);
      if (lookahead != 0 &&
          (lookahead < '\t' || '\r' < lookahead) &&
          lookahead != ' ' &&
          lookahead != '"' &&
          (lookahead < '\'' || ')' < lookahead) &&
          lookahead != ';') ADVANCE(16);
      END_STATE();
    case 4:
      if (lookahead != 0 &&
          lookahead != '\n') ADVANCE(1);
      END_STATE();
    case 5:
      if (lookahead != 0 &&
          lookahead != '\n') ADVANCE(2);
      END_STATE();
    case 6:
      if (eof) ADVANCE(7);
      if (lookahead == '"') ADVANCE(1);
      if (lookahead == '\'') ADVANCE(2);
      if (lookahead == '(') ADVANCE(8);
      if (lookahead == '-') ADVANCE(14);
      if (lookahead == '.') ADVANCE(3);
      if (lookahead == '0') ADVANCE(12);
      if (lookahead == ';') ADVANCE(18);
      if (('\t' <= lookahead && lookahead <= '\r') ||
          lookahead == ' ') SKIP(6);
      if (('1' <= lookahead && lookahead <= '9')) ADVANCE(13);
      if (lookahead != 0 &&
          (lookahead < '\'' || ')' < lookahead)) ADVANCE(16);
      END_STATE();
    case 7:
      ACCEPT_TOKEN(ts_builtin_sym_end);
      END_STATE();
    case 8:
      ACCEPT_TOKEN(anon_sym_LPAREN);
      END_STATE();
    case 9:
      ACCEPT_TOKEN(anon_sym_RPAREN);
      END_STATE();
    case 10:
      ACCEPT_TOKEN(sym_dot);
      END_STATE();
    case 11:
      ACCEPT_TOKEN(sym_hex);
      if (('0' <= lookahead && lookahead <= '9')) ADVANCE(11);
      if (('A' <= lookahead && lookahead <= 'F') ||
          ('a' <= lookahead && lookahead <= 'f')) ADVANCE(11);
      END_STATE();
    case 12:
      ACCEPT_TOKEN(sym_number);
      if (lookahead == 'X' ||
          lookahead == 'x') ADVANCE(15);
      if (('0' <= lookahead && lookahead <= '9')) ADVANCE(13);
      END_STATE();
    case 13:
      ACCEPT_TOKEN(sym_number);
      if (('0' <= lookahead && lookahead <= '9')) ADVANCE(13);
      END_STATE();
    case 14:
      ACCEPT_TOKEN(sym_symbol);
      if (lookahead == '.') ADVANCE(16);
      if (('0' <= lookahead && lookahead <= '9')) ADVANCE(13);
      if (lookahead != 0 &&
          (lookahead < '\t' || '\r' < lookahead) &&
          lookahead != ' ' &&
          lookahead != '"' &&
          (lookahead < '\'' || ')' < lookahead) &&
          lookahead != ';') ADVANCE(16);
      END_STATE();
    case 15:
      ACCEPT_TOKEN(sym_symbol);
      if (lookahead == '.') ADVANCE(16);
      if (('0' <= lookahead && lookahead <= '9')) ADVANCE(11);
      if (('A' <= lookahead && lookahead <= 'F') ||
          ('a' <= lookahead && lookahead <= 'f')) ADVANCE(11);
      if (lookahead != 0 &&
          (lookahead < '\t' || '\r' < lookahead) &&
          lookahead != ' ' &&
          lookahead != '"' &&
          (lookahead < '\'' || ')' < lookahead) &&
          lookahead != ';') ADVANCE(16);
      END_STATE();
    case 16:
      ACCEPT_TOKEN(sym_symbol);
      if (lookahead == '.' ||
          ('0' <= lookahead && lookahead <= '9')) ADVANCE(16);
      if (lookahead != 0 &&
          (lookahead < '\t' || '\r' < lookahead) &&
          lookahead != ' ' &&
          lookahead != '"' &&
          (lookahead < '\'' || ')' < lookahead) &&
          lookahead != ';') ADVANCE(16);
      END_STATE();
    case 17:
      ACCEPT_TOKEN(sym_string);
      END_STATE();
    case 18:
      ACCEPT_TOKEN(sym_comment);
      if (lookahead != 0 &&
          lookahead != '\n') ADVANCE(18);
      END_STATE();
    default:
      return false;
  }
}

static const TSLexMode ts_lex_modes[STATE_COUNT] = {
  [0] = {.lex_state = 0},
  [1] = {.lex_state = 6},
  [2] = {.lex_state = 0},
  [3] = {.lex_state = 0},
  [4] = {.lex_state = 0},
  [5] = {.lex_state = 6},
  [6] = {.lex_state = 0},
  [7] = {.lex_state = 0},
  [8] = {.lex_state = 0},
  [9] = {.lex_state = 0},
  [10] = {.lex_state = 6},
  [11] = {.lex_state = 6},
  [12] = {.lex_state = 0},
  [13] = {.lex_state = 0},
  [14] = {.lex_state = 0},
};

static const uint16_t ts_parse_table[LARGE_STATE_COUNT][SYMBOL_COUNT] = {
  [0] = {
    [ts_builtin_sym_end] = ACTIONS(1),
    [anon_sym_LPAREN] = ACTIONS(1),
    [anon_sym_RPAREN] = ACTIONS(1),
    [sym_dot] = ACTIONS(1),
    [sym_hex] = ACTIONS(1),
    [sym_number] = ACTIONS(1),
    [sym_symbol] = ACTIONS(1),
    [sym_string] = ACTIONS(1),
    [sym_comment] = ACTIONS(3),
  },
  [1] = {
    [sym_source_file] = STATE(12),
    [sym__sexp] = STATE(5),
    [sym_list] = STATE(5),
    [aux_sym_source_file_repeat1] = STATE(5),
    [ts_builtin_sym_end] = ACTIONS(5),
    [anon_sym_LPAREN] = ACTIONS(7),
    [sym_hex] = ACTIONS(9),
    [sym_number] = ACTIONS(11),
    [sym_symbol] = ACTIONS(11),
    [sym_string] = ACTIONS(9),
    [sym_comment] = ACTIONS(3),
  },
  [2] = {
    [sym__sexp] = STATE(2),
    [sym_list] = STATE(2),
    [aux_sym_source_file_repeat1] = STATE(2),
    [ts_builtin_sym_end] = ACTIONS(13),
    [anon_sym_LPAREN] = ACTIONS(15),
    [anon_sym_RPAREN] = ACTIONS(13),
    [sym_dot] = ACTIONS(13),
    [sym_hex] = ACTIONS(18),
    [sym_number] = ACTIONS(21),
    [sym_symbol] = ACTIONS(21),
    [sym_string] = ACTIONS(18),
    [sym_comment] = ACTIONS(3),
  },
  [3] = {
    [sym__sexp] = STATE(4),
    [sym_list] = STATE(4),
    [aux_sym_source_file_repeat1] = STATE(4),
    [anon_sym_LPAREN] = ACTIONS(7),
    [anon_sym_RPAREN] = ACTIONS(24),
    [sym_dot] = ACTIONS(26),
    [sym_hex] = ACTIONS(28),
    [sym_number] = ACTIONS(30),
    [sym_symbol] = ACTIONS(30),
    [sym_string] = ACTIONS(28),
    [sym_comment] = ACTIONS(3),
  },
  [4] = {
    [sym__sexp] = STATE(2),
    [sym_list] = STATE(2),
    [aux_sym_source_file_repeat1] = STATE(2),
    [anon_sym_LPAREN] = ACTIONS(7),
    [anon_sym_RPAREN] = ACTIONS(32),
    [sym_dot] = ACTIONS(34),
    [sym_hex] = ACTIONS(36),
    [sym_number] = ACTIONS(38),
    [sym_symbol] = ACTIONS(38),
    [sym_string] = ACTIONS(36),
    [sym_comment] = ACTIONS(3),
  },
  [5] = {
    [sym__sexp] = STATE(2),
    [sym_list] = STATE(2),
    [aux_sym_source_file_repeat1] = STATE(2),
    [ts_builtin_sym_end] = ACTIONS(40),
    [anon_sym_LPAREN] = ACTIONS(7),
    [sym_hex] = ACTIONS(36),
    [sym_number] = ACTIONS(38),
    [sym_symbol] = ACTIONS(38),
    [sym_string] = ACTIONS(36),
    [sym_comment] = ACTIONS(3),
  },
  [6] = {
    [ts_builtin_sym_end] = ACTIONS(42),
    [anon_sym_LPAREN] = ACTIONS(42),
    [anon_sym_RPAREN] = ACTIONS(42),
    [sym_dot] = ACTIONS(42),
    [sym_hex] = ACTIONS(42),
    [sym_number] = ACTIONS(44),
    [sym_symbol] = ACTIONS(44),
    [sym_string] = ACTIONS(42),
    [sym_comment] = ACTIONS(3),
  },
  [7] = {
    [ts_builtin_sym_end] = ACTIONS(46),
    [anon_sym_LPAREN] = ACTIONS(46),
    [anon_sym_RPAREN] = ACTIONS(46),
    [sym_dot] = ACTIONS(46),
    [sym_hex] = ACTIONS(46),
    [sym_number] = ACTIONS(48),
    [sym_symbol] = ACTIONS(48),
    [sym_string] = ACTIONS(46),
    [sym_comment] = ACTIONS(3),
  },
  [8] = {
    [ts_builtin_sym_end] = ACTIONS(50),
    [anon_sym_LPAREN] = ACTIONS(50),
    [anon_sym_RPAREN] = ACTIONS(50),
    [sym_dot] = ACTIONS(50),
    [sym_hex] = ACTIONS(50),
    [sym_number] = ACTIONS(52),
    [sym_symbol] = ACTIONS(52),
    [sym_string] = ACTIONS(50),
    [sym_comment] = ACTIONS(3),
  },
  [9] = {
    [ts_builtin_sym_end] = ACTIONS(54),
    [anon_sym_LPAREN] = ACTIONS(54),
    [anon_sym_RPAREN] = ACTIONS(54),
    [sym_dot] = ACTIONS(54),
    [sym_hex] = ACTIONS(54),
    [sym_number] = ACTIONS(56),
    [sym_symbol] = ACTIONS(56),
    [sym_string] = ACTIONS(54),
    [sym_comment] = ACTIONS(3),
  },
  [10] = {
    [sym__sexp] = STATE(13),
    [sym_list] = STATE(13),
    [anon_sym_LPAREN] = ACTIONS(7),
    [sym_hex] = ACTIONS(58),
    [sym_number] = ACTIONS(60),
    [sym_symbol] = ACTIONS(60),
    [sym_string] = ACTIONS(58),
    [sym_comment] = ACTIONS(3),
  },
  [11] = {
    [sym__sexp] = STATE(14),
    [sym_list] = STATE(14),
    [anon_sym_LPAREN] = ACTIONS(7),
    [sym_hex] = ACTIONS(62),
    [sym_number] = ACTIONS(64),
    [sym_symbol] = ACTIONS(64),
    [sym_string] = ACTIONS(62),
    [sym_comment] = ACTIONS(3),
  },
};

static const uint16_t ts_small_parse_table[] = {
  [0] = 2,
    ACTIONS(3), 1,
      sym_comment,
    ACTIONS(66), 1,
      ts_builtin_sym_end,
  [7] = 2,
    ACTIONS(3), 1,
      sym_comment,
    ACTIONS(68), 1,
      anon_sym_RPAREN,
  [14] = 2,
    ACTIONS(3), 1,
      sym_comment,
    ACTIONS(70), 1,
      anon_sym_RPAREN,
};

static const uint32_t ts_small_parse_table_map[] = {
  [SMALL_STATE(12)] = 0,
  [SMALL_STATE(13)] = 7,
  [SMALL_STATE(14)] = 14,
};

static const TSParseActionEntry ts_parse_actions[] = {
  [0] = {.entry = {.count = 0, .reusable = false}},
  [1] = {.entry = {.count = 1, .reusable = false}}, RECOVER(),
  [3] = {.entry = {.count = 1, .reusable = true}}, SHIFT_EXTRA(),
  [5] = {.entry = {.count = 1, .reusable = true}}, REDUCE(sym_source_file, 0, 0, 0),
  [7] = {.entry = {.count = 1, .reusable = true}}, SHIFT(3),
  [9] = {.entry = {.count = 1, .reusable = true}}, SHIFT(5),
  [11] = {.entry = {.count = 1, .reusable = false}}, SHIFT(5),
  [13] = {.entry = {.count = 1, .reusable = true}}, REDUCE(aux_sym_source_file_repeat1, 2, 0, 0),
  [15] = {.entry = {.count = 2, .reusable = true}}, REDUCE(aux_sym_source_file_repeat1, 2, 0, 0), SHIFT_REPEAT(3),
  [18] = {.entry = {.count = 2, .reusable = true}}, REDUCE(aux_sym_source_file_repeat1, 2, 0, 0), SHIFT_REPEAT(2),
  [21] = {.entry = {.count = 2, .reusable = false}}, REDUCE(aux_sym_source_file_repeat1, 2, 0, 0), SHIFT_REPEAT(2),
  [24] = {.entry = {.count = 1, .reusable = true}}, SHIFT(6),
  [26] = {.entry = {.count = 1, .reusable = true}}, SHIFT(10),
  [28] = {.entry = {.count = 1, .reusable = true}}, SHIFT(4),
  [30] = {.entry = {.count = 1, .reusable = false}}, SHIFT(4),
  [32] = {.entry = {.count = 1, .reusable = true}}, SHIFT(7),
  [34] = {.entry = {.count = 1, .reusable = true}}, SHIFT(11),
  [36] = {.entry = {.count = 1, .reusable = true}}, SHIFT(2),
  [38] = {.entry = {.count = 1, .reusable = false}}, SHIFT(2),
  [40] = {.entry = {.count = 1, .reusable = true}}, REDUCE(sym_source_file, 1, 0, 0),
  [42] = {.entry = {.count = 1, .reusable = true}}, REDUCE(sym_list, 2, 0, 0),
  [44] = {.entry = {.count = 1, .reusable = false}}, REDUCE(sym_list, 2, 0, 0),
  [46] = {.entry = {.count = 1, .reusable = true}}, REDUCE(sym_list, 3, 0, 0),
  [48] = {.entry = {.count = 1, .reusable = false}}, REDUCE(sym_list, 3, 0, 0),
  [50] = {.entry = {.count = 1, .reusable = true}}, REDUCE(sym_list, 4, 0, 0),
  [52] = {.entry = {.count = 1, .reusable = false}}, REDUCE(sym_list, 4, 0, 0),
  [54] = {.entry = {.count = 1, .reusable = true}}, REDUCE(sym_list, 5, 0, 0),
  [56] = {.entry = {.count = 1, .reusable = false}}, REDUCE(sym_list, 5, 0, 0),
  [58] = {.entry = {.count = 1, .reusable = true}}, SHIFT(13),
  [60] = {.entry = {.count = 1, .reusable = false}}, SHIFT(13),
  [62] = {.entry = {.count = 1, .reusable = true}}, SHIFT(14),
  [64] = {.entry = {.count = 1, .reusable = false}}, SHIFT(14),
  [66] = {.entry = {.count = 1, .reusable = true}},  ACCEPT_INPUT(),
  [68] = {.entry = {.count = 1, .reusable = true}}, SHIFT(8),
  [70] = {.entry = {.count = 1, .reusable = true}}, SHIFT(9),
};

#ifdef __cplusplus
extern "C" {
#endif
#ifdef TREE_SITTER_HIDE_SYMBOLS
#define TS_PUBLIC
#elif defined(_WIN32)
#define TS_PUBLIC __declspec(dllexport)
#else
#define TS_PUBLIC __attribute__((visibility("default")))
#endif

TS_PUBLIC const TSLanguage *tree_sitter_chialisp(void) {
  static const TSLanguage language = {
    .version = LANGUAGE_VERSION,
    .symbol_count = SYMBOL_COUNT,
    .alias_count = ALIAS_COUNT,
    .token_count = TOKEN_COUNT,
    .external_token_count = EXTERNAL_TOKEN_COUNT,
    .state_count = STATE_COUNT,
    .large_state_count = LARGE_STATE_COUNT,
    .production_id_count = PRODUCTION_ID_COUNT,
    .field_count = FIELD_COUNT,
    .max_alias_sequence_length = MAX_ALIAS_SEQUENCE_LENGTH,
    .parse_table = &ts_parse_table[0][0],
    .small_parse_table = ts_small_parse_table,
    .small_parse_table_map = ts_small_parse_table_map,
    .parse_actions = ts_parse_actions,
    .symbol_names = ts_symbol_names,
    .symbol_metadata = ts_symbol_metadata,
    .public_symbol_map = ts_symbol_map,
    .alias_map = ts_non_terminal_alias_map,
    .alias_sequences = &ts_alias_sequences[0][0],
    .lex_modes = ts_lex_modes,
    .lex_fn = ts_lex,
    .primary_state_ids = ts_primary_state_ids,
  };
  return &language;
}
#ifdef __cplusplus
}
#endif
