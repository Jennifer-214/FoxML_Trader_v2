#pragma once
// LogViewerPanel — tails engine.log in a dockable panel
// auto-scrolls to bottom, refreshes on file change

#include "imgui.h"
#include "FoxmlTheme.hpp"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

static constexpr int LOG_BUF_SIZE = 32768;
static constexpr int LOG_MAX_LINES = 200;

struct LogViewer {
    char buf[LOG_BUF_SIZE];
    int buf_len;
    char path[256];
    long last_size;
    bool auto_scroll;
};

static inline void LogViewer_Init(LogViewer *lv, const char *path) {
    memset(lv, 0, sizeof(*lv));
    strncpy(lv->path, path, 255);
    lv->auto_scroll = true;
}

static inline void LogViewer_Refresh(LogViewer *lv) {
    struct stat st;
    if (stat(lv->path, &st) != 0) return;
    if (st.st_size == lv->last_size) return;
    lv->last_size = st.st_size;

    FILE *f = fopen(lv->path, "r");
    if (!f) return;

    // read last LOG_BUF_SIZE bytes
    if (st.st_size > LOG_BUF_SIZE - 1) {
        fseek(f, st.st_size - (LOG_BUF_SIZE - 1), SEEK_SET);
    }

    lv->buf_len = (int)fread(lv->buf, 1, LOG_BUF_SIZE - 1, f);
    lv->buf[lv->buf_len] = '\0';
    fclose(f);

    // skip to first complete line if we seeked into the middle
    if (st.st_size > LOG_BUF_SIZE - 1) {
        char *nl = strchr(lv->buf, '\n');
        if (nl) {
            int skip = (int)(nl - lv->buf + 1);
            memmove(lv->buf, lv->buf + skip, lv->buf_len - skip + 1);
            lv->buf_len -= skip;
        }
    }
}

static inline void GUI_Panel_LogViewer(LogViewer *lv) {
    ImGui::Begin("Engine Log");

    LogViewer_Refresh(lv);

    ImGui::TextColored(FoxmlColors::primary, "ENGINE LOG");
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::comment, "%s", lv->path);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &lv->auto_scroll);
    ImGui::Separator();

    // v5.11.20.1 — text wrap. Pre-fix the panel had
    // ImGuiWindowFlags_HorizontalScrollbar and used TextColored (which
    // does NOT wrap), so long log lines (e.g. v5.11.0.B mlockall WARN
    // citing the full /etc/security/limits.conf path) ran off the
    // right edge and required horizontal scrolling. Operator caught
    // 2026-05-07. Post-fix: drop horizontal scrollbar; render each
    // line via PushStyleColor + TextWrapped + PopStyleColor — wraps
    // at the right edge of the panel, preserving color coding.
    ImGui::BeginChild("##log_scroll", ImVec2(0, 0), ImGuiChildFlags_None,
                       ImGuiWindowFlags_None);

    if (lv->buf_len > 0) {
        // render line by line with color coding
        char *line = lv->buf;
        while (line && *line) {
            char *eol = strchr(line, '\n');
            if (eol) *eol = '\0';

            // color based on content — push as style, render TextWrapped,
            // pop. TextWrapped honors the panel width by default.
            ImVec4 color;
            if (strstr(line, "error") || strstr(line, "ERROR") || strstr(line, "FAIL"))
                color = FoxmlColors::red;
            else if (strstr(line, "warn") || strstr(line, "WARN"))
                color = FoxmlColors::yellow;
            else if (strstr(line, "FILL") || strstr(line, "BUY") || strstr(line, "SELL"))
                color = FoxmlColors::wheat;
            else if (strstr(line, "REGIME") || strstr(line, "STRATEGY"))
                color = FoxmlColors::accent;
            else
                color = FoxmlColors::comment;

            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextWrapped("%s", line);
            ImGui::PopStyleColor();

            if (eol) {
                *eol = '\n';
                line = eol + 1;
            } else {
                break;
            }
        }

        if (lv->auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20)
            ImGui::SetScrollHereY(1.0f);
    } else {
        ImGui::TextColored(FoxmlColors::comment, "no log output yet");
    }

    ImGui::EndChild();
    ImGui::End();
}
