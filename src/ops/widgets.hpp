#pragma once

#include <map>
#include <string>

namespace scopes
{
// Operator-screen widgets (SPECIFICATION §9.3): the one widget `scope`. Adapted from mxl-replay
// src/ops/preview.cpp (GPL-3.0, copied into this MIT project with the author's permission).

// GET /widgets: [{id, title, params (JSON schema), min_size, version}].
[[nodiscard]] std::string widgetsJson();
// The decoded query of /widget/<id> (`controls`, `measurements`, `theme`): empty when valid, else
// the reason (400). Unknown widgets are 404 (`knownWidget`).
[[nodiscard]] bool knownWidget(std::string const& id);
[[nodiscard]] std::string widgetQueryError(std::map<std::string, std::string> const& query);
// True when the WIDGET_FRAME_ANCESTORS source list names this Origin (or holds `*`).
[[nodiscard]] bool listedOrigin(std::string const& sources, std::string origin);
// name=value pairs of a query string, percent-decoded.
[[nodiscard]] std::map<std::string, std::string> parseQuery(std::string const& query);
} // namespace scopes
