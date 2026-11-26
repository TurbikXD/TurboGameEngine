#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/NetworkTestCsv.h"
#include "engine/core/NetworkTestReportChartScript.h"

namespace engine::core {
namespace network_report_html {

inline void appendEscaped(std::string& html, std::string_view text) {
    for (const char ch : text) {
        switch (ch) {
            case '&': html += "&amp;"; break;
            case '<': html += "&lt;"; break;
            case '>': html += "&gt;"; break;
            case '"': html += "&quot;"; break;
            case '\'': html += "&#39;"; break;
            default: html += ch; break;
        }
    }
}

inline void appendFilename(std::string& html, const std::filesystem::path& path) {
    const auto name = path.filename().u8string();
    appendEscaped(html, {reinterpret_cast<const char*>(name.data()), name.size()});
}

inline void appendBase64(std::string& html, std::string_view bytes) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto remaining = bytes.size() - i;
        const auto first = static_cast<unsigned char>(bytes[i]);
        const auto second = remaining > 1 ? static_cast<unsigned char>(bytes[i + 1]) : 0U;
        const auto third = remaining > 2 ? static_cast<unsigned char>(bytes[i + 2]) : 0U;
        const char encoded[]{alphabet[first >> 2U], alphabet[((first & 3U) << 4U) | (second >> 4U)],
            remaining > 1 ? alphabet[((second & 15U) << 2U) | (third >> 6U)] : '=',
            remaining > 2 ? alphabet[third & 63U] : '='};
        html.append(encoded, sizeof(encoded));
    }
}

inline void appendValueCell(std::string& html, std::optional<double> value, bool delta, std::string_view unit) {
    if (!value || !std::isfinite(*value)) {
        html += "<td class=\"missing\" title=\"No data\">&mdash;</td>";
        return;
    }
    const double shown = std::abs(*value) < 0.005 ? 0.0 : *value;
    const bool large = std::abs(shown) >= 1e6;
    char buffer[64]{};
    const auto formatted = std::to_chars(std::begin(buffer), std::end(buffer), shown,
        large ? std::chars_format::general : std::chars_format::fixed, large ? 3 : 2);
    if (formatted.ec != std::errc{}) throw std::runtime_error("Could not format an HTML report value.");
    html += "<td>";
    if (delta && shown > 0.0) html += '+';
    html.append(buffer, static_cast<std::size_t>(formatted.ptr - buffer));
    if (unit == "%") html += '%';
    else if (unit == "pp") html += " pp";
    html += "</td>";
}

inline void appendChartNumber(std::string& html, std::optional<double> value) {
    if (!value || !std::isfinite(*value) || *value < 0.0) {
        html += "null";
        return;
    }
    char buffer[64];
    const auto number = std::to_chars(std::begin(buffer), std::end(buffer), *value);
    if (number.ec != std::errc{}) throw std::runtime_error("Could not format an HTML chart value.");
    html.append(buffer, static_cast<std::size_t>(number.ptr - buffer));
}

inline void appendChartSample(std::string& html, const net::NetworkTestHistoryPoint& sample) {
    const auto& metrics = sample.metrics;
    html += "[\"" + std::to_string(sample.elapsedMs) + "\",";
    html += sample.breakBefore ? '1' : '0';
    const std::optional<double> values[]{metrics.rttMs, metrics.uplinkLossPercent, metrics.downlinkLossPercent,
        metrics.probeLossPercent, metrics.upload ? std::optional{metrics.upload->bytesPerSecond / 1024.0} : std::nullopt,
        metrics.download ? std::optional{metrics.download->bytesPerSecond / 1024.0} : std::nullopt,
        metrics.upload ? std::optional{metrics.upload->packetsPerSecond} : std::nullopt,
        metrics.download ? std::optional{metrics.download->packetsPerSecond} : std::nullopt};
    for (const auto& value : values) {
        html += ',';
        appendChartNumber(html, value);
    }
    html += ']';
}

inline void appendChartData(std::string& html, std::span<const NetworkTestComparisonReport* const> clients) {
    const auto& bounds = clients.front()->summary;
    html += "<script type=\"application/json\" id=\"chart-data\">{\"start\":\"" + std::to_string(bounds.rangeStartMs) +
        "\",\"end\":\"" + std::to_string(bounds.rangeEndMs) + "\",\"clients\":[";
    for (std::size_t index = 0; index < clients.size(); ++index) {
        if (index) html += ',';
        const auto& report = *clients[index];
        html += "{\"id\":" + std::to_string(report.summary.clientIndex) + ",\"runs\":[";
        for (std::size_t run = 0; run < report.chartSamples.size(); ++run) {
            if (run) html += ',';
            html += '[';
            const auto& samples = report.chartSamples[run];
            for (std::size_t i = 0; i < samples.size(); ++i) {
                if (i) html += ',';
                appendChartSample(html, samples[i]);
            }
            html += ']';
        }
        html += "]}";
    }
    html += "]}</script>\n";
}

inline constexpr std::string_view PageStart = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src data:; script-src 'unsafe-inline'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'">
<title>Network Test Comparison</title>
<style>
:root { color-scheme: light; font: 16px/1.55 Inter, system-ui, sans-serif; color: #182230; background: #f3f6fa; }
* { box-sizing: border-box; }
body { margin: 0; }
main { max-width: 1100px; margin: 0 auto; padding: clamp(16px, 4vw, 44px); }
h1, h2, h3 { margin: 0; line-height: 1.2; }
h1 { font-size: clamp(1.85rem, 4vw, 2.6rem); letter-spacing: -.035em; }
h2 { font-size: 1.4rem; }
h3 { font-size: 1.14rem; }
a { color: #2451a5; }
a:focus-visible, button:focus-visible, select:focus-visible, .plot:focus-visible { outline: 2px solid #2451a5; outline-offset: 3px; }
[hidden] { display: none !important; }
p { margin: .75rem 0 0; }
.meta { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 220px), 1fr)); gap: 16px; margin: 24px 0; }
.meta > div, section, figure { border: 1px solid #d9e0e9; background: #fff; border-radius: 10px; }
.meta > div { padding: 14px 16px; }
dt { color: #42536b; font-size: .88rem; font-weight: 650; }
dd { margin: 2px 0 0; overflow-wrap: anywhere; }
nav { display: flex; gap: 8px 18px; flex-wrap: wrap; margin-bottom: 24px; }
section { padding: clamp(14px, 3vw, 26px); margin-bottom: 24px; }
.table-scroll { overflow-x: auto; margin-top: 16px; }
table { width: 100%; min-width: 530px; border-collapse: collapse; font-variant-numeric: tabular-nums; }
th, td { padding: .42rem .65rem; border-bottom: 1px solid #e4e9f0; }
thead { color: #293a52; background: #edf2f8; }
th { text-align: left; }
td, thead th.numeric { text-align: right; }
tbody th { font-weight: 500; }
tbody th[scope="rowgroup"] { vertical-align: top; white-space: nowrap; font-weight: 650; }
tbody + tbody { border-top: 2px solid #d9e0e9; }
tbody tr:nth-child(even) { background: #f8fafc; }
.missing, .note, figcaption { color: #516179; }
.chart-controls { display: flex; align-items: center; flex-wrap: wrap; gap: 8px; margin: 14px 0 0; }
.chart-controls label { font-weight: 650; }
button, .chart-controls select { font: inherit; color: inherit; background: #f8fafc; border: 1px solid #b9c7da; border-radius: 6px; padding: .28rem .55rem; }
button { cursor: pointer; }
button:disabled { opacity: .55; cursor: default; }
.chart-controls output { font-variant-numeric: tabular-nums; overflow-wrap: anywhere; }
.summary-header { display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: 8px 16px; }
#visible-summary-download-status:empty { display: none; }
.chart-grid { display: grid; grid-template-columns: minmax(0, 1fr) minmax(235px, 28%); gap: 14px; margin-top: 12px; align-items: start; }
.plot { display: block; width: 100%; height: auto; touch-action: pan-y; margin: 0 0 8px; }
.chart-readout { position: sticky; top: 12px; border: 1px solid #d9e0e9; border-radius: 8px; padding: 10px; font-size: .85rem; overflow-wrap: anywhere; }
.chart-readout table { min-width: 0; }
.chart-readout th, .chart-readout td { padding: .24rem .25rem; }
#chart-status:empty { display: none; }
details > summary { cursor: pointer; color: #2451a5; font-weight: 650; }
figure { padding: 14px; margin: 16px 0 0; }
figure img { display: block; width: 100%; height: auto; margin: 12px 0 8px; border-radius: 6px; }
figcaption, .note { font-size: .9rem; }
@media (max-width: 760px) {
  .chart-grid { display: flex; flex-direction: column; }
  .chart-grid > div { width: 100%; }
  .chart-readout { position: static; width: 100%; }
}
@media print {
  :root { background: #fff; }
  main { max-width: none; padding: 0; }
  nav, #visible-summary-download, #visible-summary-download-status { display: none; }
  table { min-width: 0; }
  figure, tr { break-inside: avoid; }
}
</style>
</head>
<body>
<main>
<header>
<h1>Network Test Comparison</h1>
)HTML";

inline constexpr std::string_view ChartPanel = R"HTML(<section id="interactive-charts" aria-labelledby="interactive-heading" hidden>
<h2 id="interactive-heading">Interactive charts</h2>
<div class="chart-controls">
  <label for="chart-client">Client</label><select id="chart-client"></select>
  <button type="button" id="chart-zoom-in">Zoom in</button>
  <button type="button" id="chart-zoom-out">Zoom out</button>
  <button type="button" id="chart-reset">Reset zoom</button>
  <output id="chart-window" aria-label="Visible elapsed time"></output>
</div>
<p id="chart-help" class="note">Drag across a plot to zoom. Hover over a plot to compare nearby samples from both runs. Run A is solid. Run B is dashed.</p>
<p id="chart-status" class="note" role="status"></p>
<div class="chart-grid">
  <div>
    <svg id="chart-rtt" class="plot" aria-describedby="chart-help"></svg>
    <svg id="chart-loss" class="plot" aria-describedby="chart-help"></svg>
    <svg id="chart-traffic" class="plot" aria-describedby="chart-help"></svg>
  </div>
  <aside id="chart-tooltip" class="chart-readout" aria-label="Sample values"></aside>
</div>
</section>
)HTML";

inline constexpr std::string_view VisibleSummaryPanel = R"HTML(<section id="visible-summary" aria-labelledby="visible-summary-heading" hidden>
<div class="summary-header">
  <h2 id="visible-summary-heading">Visible range summary</h2>
  <button type="button" id="visible-summary-download">Download CSV</button>
</div>
<p class="note" id="visible-summary-window" role="status"></p>
<p class="note" id="visible-summary-download-status" role="status"></p>
<p class="note" id="visible-summary-note">Means use available samples. p95 RTT uses the nearest-rank percentile. Loss differences are in percentage points (pp). A dash indicates missing data.</p>
<div class="table-scroll" role="region" aria-labelledby="visible-summary-heading" tabindex="0">
  <table aria-labelledby="visible-summary-heading" aria-describedby="visible-summary-note">
    <thead><tr><th scope="col">Metric</th><th scope="col" class="numeric">Run A</th><th scope="col" class="numeric">Run B</th><th scope="col" class="numeric">B - A</th></tr></thead>
    <tbody id="visible-summary-rows"></tbody>
  </table>
</div>
</section>
)HTML";

}

[[nodiscard]] inline std::string renderNetworkTestReportHtml(std::span<const NetworkTestComparisonReport> reports) {
    using namespace network_report_html;
    if (reports.empty()) throw std::invalid_argument("No clients in the HTML report.");

    std::vector<const NetworkTestComparisonReport*> clients;
    clients.reserve(reports.size());
    for (const auto& report : reports) clients.push_back(&report);
    std::ranges::sort(clients, {}, [](const auto* report) { return report->summary.clientIndex; });

    std::string html(PageStart);
    const auto& selection = clients.front()->summary;
    html += "<dl class=\"meta\">\n<div><dt>Run A</dt><dd id=\"report-run-a\">";
    appendFilename(html, selection.runAPath);
    html += "</dd></div>\n<div><dt>Run B</dt><dd id=\"report-run-b\">";
    appendFilename(html, selection.runBPath);
    html += "</dd></div>\n<div><dt>Elapsed time</dt><dd>";
    appendEscaped(html, selection.rangeLabel);
    html += " &middot; " + std::to_string(selection.rangeStartMs) + " &ndash; " +
        std::to_string(selection.rangeEndMs) + " ms</dd></div>\n</dl>\n</header>\n";
    html += "<nav aria-label=\"Report sections\"><a href=\"#summary\">Summary</a>";
    for (const auto* report : clients) {
        const auto id = std::to_string(report->summary.clientIndex);
        html += "<a href=\"#client-" + id + "\" data-chart-client=\"" + id + "\">Client " + id + "</a>";
    }
    html += "</nav>\n";
    html += ChartPanel;
    html += VisibleSummaryPanel;
    html += "<section id=\"summary\" aria-labelledby=\"summary-heading\">\n<h2 id=\"summary-heading\">Exported range summary</h2>\n"
        "<p class=\"note\" id=\"summary-note\">B - A is the difference between runs. Loss differences use percentage points (pp). "
        "A dash indicates missing data.</p>\n"
        "<div class=\"table-scroll\" role=\"region\" aria-labelledby=\"summary-heading\" tabindex=\"0\">\n"
        "<table aria-labelledby=\"summary-heading\" aria-describedby=\"summary-note\">\n<thead><tr>"
        "<th scope=\"col\">Client</th><th scope=\"col\">Metric</th><th scope=\"col\" class=\"numeric\">Run A</th>"
        "<th scope=\"col\" class=\"numeric\">Run B</th><th scope=\"col\" class=\"numeric\">B - A</th></tr></thead>\n";
    for (const auto* report : clients) {
        const auto& summary = report->summary;
        const auto id = std::to_string(summary.clientIndex);
        html += "<tbody>\n<tr><th scope=\"rowgroup\" rowspan=\"" + std::to_string(std::size(net::NetworkTestSummaryMetrics)) +
            "\"><a href=\"#client-" + id + "\" data-chart-client=\"" + id + "\">Client " + id + "</a></th>";
        for (const auto& metric : net::NetworkTestSummaryMetrics) {
            if (&metric != std::begin(net::NetworkTestSummaryMetrics)) html += "<tr>";
            html += "<th scope=\"row\">";
            appendEscaped(html, metric.label);
            html += "</th>";
            const auto values = metric.values(summary.runA, summary.runB);
            appendValueCell(html, values.runA, false, metric.unit);
            appendValueCell(html, values.runB, false, metric.unit);
            appendValueCell(html, values.delta, true, metric.deltaUnit);
            html += "</tr>\n";
        }
        html += "</tbody>\n";
    }
    html += "</table>\n</div>\n</section>\n<section aria-label=\"Static charts\">\n"
        "<details id=\"static-charts\" open><summary>Static charts</summary>\n";
    for (const auto* report : clients) {
        const auto id = std::to_string(report->summary.clientIndex);
        html += "<figure id=\"client-" + id + "\">\n<h3>Client " + id + "</h3>\n"
            "<img width=\"1440\" height=\"960\" loading=\"lazy\" decoding=\"async\" alt=\"RTT, packet loss and UDP traffic charts for Client " +
            id + " comparing Run A and Run B\" src=\"data:image/png;base64,";
        appendBase64(html, report->pngBytes);
        html += "\">\n<figcaption>Run A is solid. Run B is dashed.</figcaption>\n</figure>\n";
    }
    html += "</details>\n</section>\n</main>\n";
    appendChartData(html, clients);
    html += "<script>\n";
    for (const auto fragment : ChartScript) html += fragment;
    html += "\n</script>\n</body>\n</html>\n";
    return html;
}

}
