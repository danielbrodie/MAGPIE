#ifndef MAGPIE_CROSSPLAY_ORACLE_PUBLIC_H
#define MAGPIE_CROSSPLAY_ORACLE_PUBLIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAGPIE_CROSSPLAY_ABI_VERSION 1U
#define MAGPIE_CROSSPLAY_BOARD_CELLS 225U
#define MAGPIE_CROSSPLAY_RACK_CAPACITY 7U
#define MAGPIE_CROSSPLAY_WORD_CAPACITY 15U
#define MAGPIE_CROSSPLAY_ERROR_MESSAGE_CAPACITY 512U
#define MAGPIE_CROSSPLAY_POSITION_TILE_CAPACITY 128U
#define MAGPIE_CROSSPLAY_TRANSITION_CAPACITY 128U

typedef uint32_t MagpieCrossplayStatus;

#define MAGPIE_CROSSPLAY_STATUS_OK 0U
#define MAGPIE_CROSSPLAY_STATUS_INVALID_ARGUMENT 1U
#define MAGPIE_CROSSPLAY_STATUS_ASSET_MISMATCH 2U
#define MAGPIE_CROSSPLAY_STATUS_POSITION_INVALID 3U
#define MAGPIE_CROSSPLAY_STATUS_CAPACITY_EXCEEDED 4U
#define MAGPIE_CROSSPLAY_STATUS_ENGINE_ERROR 5U

typedef uint8_t MagpieCrossplayActionKind;

#define MAGPIE_CROSSPLAY_ACTION_PLACEMENT 0U
#define MAGPIE_CROSSPLAY_ACTION_EXCHANGE 1U
#define MAGPIE_CROSSPLAY_ACTION_PASS 2U

typedef uint8_t MagpieCrossplayOrientation;

#define MAGPIE_CROSSPLAY_ORIENTATION_HORIZONTAL 0U
#define MAGPIE_CROSSPLAY_ORIENTATION_VERTICAL 1U
#define MAGPIE_CROSSPLAY_ORIENTATION_NONE 255U

// Input bytes are borrowed only for the duration of the call. Text spans must
// be UTF-8/ASCII as documented by the field and must not contain an interior
// NUL byte.
typedef struct MagpieCrossplayBytes {
  const uint8_t *data;
  uint64_t length;
} MagpieCrossplayBytes;

typedef struct MagpieCrossplayError {
  MagpieCrossplayStatus status;
  uint32_t detail;
  uint64_t message_length;
  uint8_t message[MAGPIE_CROSSPLAY_ERROR_MESSAGE_CAPACITY];
} MagpieCrossplayError;

typedef struct MagpieCrossplayAssets {
  // MAGPIE search path, for example "./testdata:./data".
  MagpieCrossplayBytes data_paths;
  // crossplay-oracle-assets-v1 manifest path.
  MagpieCrossplayBytes manifest_path;
} MagpieCrossplayAssets;

// letter is 0 for an empty cell or 1-26 for A-Z. is_blank is 0 or 1 and may
// only be 1 when letter is nonzero.
typedef struct MagpieCrossplayBoardCell {
  uint8_t letter;
  uint8_t is_blank;
  uint8_t reserved[2];
} MagpieCrossplayBoardCell;

// Rack and bag tile bytes are 0 for a blank or 1-26 for A-Z. The complete
// board+racks+bag multiset must exactly equal the pinned distribution.
typedef struct MagpieCrossplayPosition {
  const MagpieCrossplayBoardCell *board_cells;
  uint64_t board_cell_count;
  MagpieCrossplayBytes player_racks[2];
  MagpieCrossplayBytes bag;
  int32_t scores[2];
  uint8_t player_on_turn;
  uint8_t starting_player;
  uint8_t consecutive_scoreless_turns;
  uint8_t reserved[5];
} MagpieCrossplayPosition;

typedef struct MagpieCrossplayPlacementTile {
  uint8_t row;
  uint8_t col;
  uint8_t letter;
  uint8_t is_blank;
} MagpieCrossplayPlacementTile;

// All action storage is owned by the returned action set. Unused fixed-width
// fields are zeroed. Action IDs and set digests are raw SHA-256 bytes.
// native_generation/native_index are opaque session-local handles. They remain
// valid only until the next magpie_crossplay_generate_actions call on that
// session and must not be persisted as part of an action's stable identity.
typedef struct MagpieCrossplayAction {
  MagpieCrossplayActionKind kind;
  MagpieCrossplayOrientation orientation;
  uint8_t start_row;
  uint8_t start_col;
  int32_t score;
  uint8_t word_length;
  uint8_t new_tile_count;
  uint8_t exchange_count;
  uint8_t reserved;
  uint8_t word[MAGPIE_CROSSPLAY_WORD_CAPACITY];
  MagpieCrossplayPlacementTile
      new_tiles[MAGPIE_CROSSPLAY_RACK_CAPACITY];
  uint8_t exchange_tiles[MAGPIE_CROSSPLAY_RACK_CAPACITY];
  uint8_t id[32];
  uint64_t native_generation;
  uint64_t native_index;
} MagpieCrossplayAction;

typedef struct MagpieCrossplayActionSet {
  MagpieCrossplayAction *actions;
  uint64_t count;
  uint64_t placements;
  uint64_t exchanges;
  uint64_t passes;
  uint8_t digest[32];
  uint8_t complete;
  uint8_t reserved[7];
} MagpieCrossplayActionSet;

typedef struct MagpieCrossplayOwnedPosition {
  MagpieCrossplayBoardCell board_cells[MAGPIE_CROSSPLAY_BOARD_CELLS];
  uint8_t player_racks[2][MAGPIE_CROSSPLAY_RACK_CAPACITY];
  uint8_t player_rack_lengths[2];
  uint8_t bag[MAGPIE_CROSSPLAY_POSITION_TILE_CAPACITY];
  uint64_t bag_length;
  int32_t scores[2];
  uint8_t player_on_turn;
  uint8_t starting_player;
  uint8_t consecutive_scoreless_turns;
  uint8_t reserved[5];
} MagpieCrossplayOwnedPosition;

typedef struct MagpieCrossplayTransition {
  MagpieCrossplayOwnedPosition position;
  int64_t weight;
  uint8_t draw[MAGPIE_CROSSPLAY_RACK_CAPACITY];
  uint8_t draw_count;
  uint8_t bag_emptied;
  uint8_t reserved[6];
} MagpieCrossplayTransition;

typedef struct MagpieCrossplayTransitionSet {
  MagpieCrossplayTransition *transitions;
  uint64_t count;
  int64_t weight_mass;
  uint8_t complete;
  uint8_t reserved[7];
} MagpieCrossplayTransitionSet;

typedef struct MagpieCrossplayOracle MagpieCrossplayOracle;

uint32_t magpie_crossplay_abi_version(void);

MagpieCrossplayStatus magpie_crossplay_oracle_create(
    const MagpieCrossplayAssets *assets, MagpieCrossplayOracle **out_oracle,
    MagpieCrossplayError *error);

void magpie_crossplay_oracle_destroy(MagpieCrossplayOracle *oracle);

MagpieCrossplayStatus magpie_crossplay_generate_actions(
    MagpieCrossplayOracle *oracle, const MagpieCrossplayPosition *position,
    uint8_t allow_exchanges, MagpieCrossplayActionSet *out_actions,
    MagpieCrossplayError *error);

void magpie_crossplay_action_set_destroy(MagpieCrossplayActionSet *actions);

MagpieCrossplayStatus magpie_crossplay_apply_action(
    MagpieCrossplayOracle *oracle, uint64_t native_generation,
    uint64_t native_index, MagpieCrossplayTransitionSet *out_transitions,
    MagpieCrossplayError *error);

void magpie_crossplay_transition_set_destroy(
    MagpieCrossplayTransitionSet *transitions);

#ifdef __cplusplus
}
#endif

#endif
