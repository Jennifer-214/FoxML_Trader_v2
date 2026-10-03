// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[Backtest/SuiteModal.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the suite's modal windows, one way — ONE call opens a modal while something is pending (never over another popup) and begins it (centred on the viewport each time it appears, never saved to foxml_suite.ini) — and the suite's ONE launch-failure surface, built on it]
// [CONTAINS]
//   - [FUNCTION]_[SuiteModal_Begin]
//   - [STRUCT]_[LaunchFailureState]
//   - [FUNCTION]_[LaunchFailure_Modal]
// [REFERENCE]_[DECISION]_[[D-507]]
//======================================================================================================
//
// WHY ONE WAY (D-507 9.3; its review's F1 / F3): left to itself, ImGui centres a modal only the first time it EVER
// appears and saves where it last stood to the ini — so a modal first shown on a large display comes back, on a
// smaller one, with its buttons off-screen, over a suite that takes no other click until it is answered. And opening a
// popup closes any other popup open at the same level: two modals each opened while something is pending would close
// each other every frame, each hidden while it measures, the input blocked. Every suite modal opens and begins through
// SuiteModal_Begin (Class 68).
//
// WHY ONE CALL (its second review's R2): the open and the begin hash their ID on the ID stack where each is called — an
// open inside a table, child or PushID scope and a begin outside it never meet (the Past Runs delete modal's v5.11.51 /
// v5.15.5.F.6 history). Split, such an open would also leave an entry nothing begins, and an open that waits for the
// level to be free waits on it for the session — every suite modal held shut, the launch-failure window included. One
// call has no second half to misplace.
//
// ImGui only (no SDL / OpenGL): the gui lanes' headless cells drive these on ImGui's null backend.

#pragma once

#include "imgui.h"
#include "../GUI/FoxmlTheme.hpp"

//======================================================================
// [FUNCTION]_[SuiteModal_Begin]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[open the modal `id` while `pending` holds — never over another popup — and begin it, in ONE call: centred on the main viewport each time it appears (pivot 0.5, so the centre holds once its size is measured), sized to its content, never saved to foxml_suite.ini; a button's click passes its result for its one frame; true = draw its body, then ImGui::EndPopup()]
//======================================================================
// [CODE]
//======================================================================
inline bool SuiteModal_Begin(bool pending, const char *id, bool *p_open = nullptr) {
    // the flag makes the open a no-op while ANY popup is open at this level — this one included — so it opens once per
    // appearance and waits for another to close instead of closing it
    if (pending) ImGui::OpenPopup(id, ImGuiPopupFlags_NoOpenOverExistingPopup);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    return ImGui::BeginPopupModal(id, p_open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteModal_Begin]
//======================================================================

//======================================================================
// [STRUCT]_[LaunchFailureState]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the suite's ONE launch-failure surface — why the last start did not happen, shared by the three panels that start runs; GUI thread only; "" = nothing to show]
//======================================================================
// The panels used to keep a launch line each, drawn beside some of their buttons — the Training panel's one line sat by
// the Collect buttons, so a failed Train Model, Train Multi-Horizon, Walk-Forward, HP sweep or Full Validation start
// said why off-screen (D-507 call 3). The funnel writes its refusal or failed spawn here, SuiteWorker_ReportNotStarted
// every failure before the funnel (both log the same words), and the suite shows it in ONE modal (LaunchFailure_Modal).
// The launch-funnel guard holds it ONE line. Duty 3 lets a click write the member launch_msg itself (`->launch_msg`,
// `.launch_msg`, indexed or not — no worker reads this line, so it is not the shared state Duty 3 guards). Duty 5
// holds that outside this file launch_msg names nothing else (no other member, local, pointer or near-name) and is
// mentioned only as a reporter's argument (SuiteWorker_Launch / SuiteWorker_ReportNotStarted) through a
// `LaunchFailureState *` parameter; that every reporter call outside the funnel passes one; that exactly one
// LaunchFailureState exists (foxml_suite.cpp's); and that the suite draws it every frame, at its root.
//======================================================================
// [CODE]
//======================================================================
struct LaunchFailureState {
    char launch_msg[192];
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-10-02]
//----------------------------------------------------------------------
// [SIZE]_[192B]
// [ALIGN]_[1]
// [CACHE_LINES]_[3]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[LaunchFailureState]
//======================================================================

// The launch-failure window's name: its title, then a suffix (after ##) that keeps its ID its own.
static constexpr const char *LAUNCH_FAILURE_MODAL_ID = "A start did not happen##suite_launch_failure";

//======================================================================
// [FUNCTION]_[LaunchFailure_Modal]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the suite's one launch-failure window — opens while a reason is pending, names it (wrapped, so a long one reads whole on a small display), and OK forgets it; called every frame, as a bare statement at the suite's root, after every panel that starts a run]
//======================================================================
// The modal blocks every other click until it is answered, so a successful start needs no clear: OK is the one clear.
// Drawn every frame, never inside a body a panel or a condition can skip: a skipped frame leaves a reason waiting
// unseen, and a draw that stops while the window is up leaves a modal on the stack that blocks every click unseen.
//======================================================================
// [CODE]
//======================================================================
inline void LaunchFailure_Modal(LaunchFailureState *lf) {
    if (!lf) return;
    if (SuiteModal_Begin(lf->launch_msg[0] != '\0', LAUNCH_FAILURE_MODAL_ID)) {
        // wrapped at 35 font sizes — a width that scales with the font (about 58 characters of the suite's 18 px mono
        // font): a long reason grows the window down; unwrapped, ImGui clamps the window to the display and clips it
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextColored(FoxmlColors::red, "%s", lf->launch_msg);
        ImGui::PopTextWrapPos();
        ImGui::TextDisabled("(the same words are in the Engine Log — logging/foxml_suite.log)");
        if (ImGui::Button("OK")) {
            lf->launch_msg[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[LaunchFailure_Modal]
//======================================================================
