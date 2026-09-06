// SPDX-License-Identifier: AGPL-3.0-or-later

//======================================================================================================
// [FILE]_[DataStream/DepthGapReasons.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the depth recorder's "# GAP … reason=<X>" vocabulary — six H21 string-consts (append-only + immutable; `gap-reason` rows in the identifier ledger). Its own header because BinanceDepth.hpp and DepthRecorder.hpp include each other (the cycle break at the bottom of BinanceDepth.hpp): the constants must be visible in EITHER include order, and the depth thread's templates name them as non-dependent identifiers]
// [CONTAINS]
//   - the DEPTH_GAP_REASON_* string-consts (no unit blocks — constants ride the file block)
//======================================================================================================
#ifndef DEPTH_GAP_REASONS_HPP
#define DEPTH_GAP_REASONS_HPP

// The "# GAP … reason=<X>" vocabulary is a persisted, replay-visible interface (H21): append-only +
// immutable; enrolled as `string-const` rows in the identifier ledger (tools/check_identifier_retirement.py).
// A new reason = a new constant + a new row; never re-spell one. Consumers: DepthRecorder_Write (the two
// in-recorder detectors) and the depth thread's ONE DepthStream_Disconnect (BinanceDepth.hpp).
static const char DEPTH_GAP_REASON_DISCONNECT[]        = "disconnect";          // EOF / transport error / the server's close frame
static const char DEPTH_GAP_REASON_ID_BACKWARD[]       = "id_backward";         // lastUpdateId went backward between rows
static const char DEPTH_GAP_REASON_WALLCLOCK_GAP[]     = "wallclock_gap";       // > 2 s between consecutive rows
static const char DEPTH_GAP_REASON_STALE[]             = "stale";               // the watchdog: no frame for DEPTH_STALE_THRESHOLD_US
static const char DEPTH_GAP_REASON_FRAME_TOO_LARGE[]   = "frame_too_large";     // a frame larger than the read buffer — the stream desynced
static const char DEPTH_GAP_REASON_PLANNED_RECONNECT[] = "planned_reconnect";   // the 23h30m proactive reconnect (WS_PLANNED_RECONNECT_S)

#endif // DEPTH_GAP_REASONS_HPP
