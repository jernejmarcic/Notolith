// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A real diagram tool (mmdr, a JVM for PlantUML, TeX) on a machine that is
// busy with other builds can overrun the renderer's kill timeout. That says
// nothing about the renderer, so such an answer is asked for again, with a
// fresh renderer (the first one remembers its failure); every other answer,
// a broken render included, is passed on as it is and fails its test.

#include <functional>

#include "MarkdownEditor.h"

namespace diagramtest {

using Renderer = noteAppC::MarkdownEditor::CodeBlockRenderer;
using Result = noteAppC::MarkdownEditor::CodeBlockResult;

inline void askAgainOnTimeout(const std::function<Renderer()> &make, const QString &language, const QString &source,
                              int width, const QString &theme, const std::function<void(Result)> &deliver, int left)
{
    make()(language, source, width, theme, [=](Result result) {
        if (result.isMarkdown && result.text.contains(QLatin1String("timed out")) && left > 1)
            askAgainOnTimeout(make, language, source, width, theme, deliver, left - 1);
        else
            deliver(std::move(result));
    });
}

/// `make()` per call, retried up to three times when the tool timed out.
inline Renderer retryingOnTimeout(std::function<Renderer()> make)
{
    return [make = std::move(make)](const QString &language, const QString &source, int width, const QString &theme,
                                    const std::function<void(Result)> &deliver) {
        askAgainOnTimeout(make, language, source, width, theme, deliver, 3);
    };
}

} // namespace diagramtest
