#pragma once

#include <array>
#include <string_view>

namespace engine::core::network_report_html {

inline constexpr std::array<std::string_view, 2> ChartScript{R"JS(
(() => {
  "use strict";

  const get = id => document.getElementById(id);
  const panel = get("interactive-charts");
  const fallback = get("static-charts");
  const select = get("chart-client");
  const zoomIn = get("chart-zoom-in");
  const zoomOut = get("chart-zoom-out");
  const reset = get("chart-reset");
  const windowLabel = get("chart-window");
  const status = get("chart-status");
  const tooltip = get("chart-tooltip");
  const summary = get("visible-summary");
  const summaryWindow = get("visible-summary-window");
  const summaryRows = get("visible-summary-rows");
  const summaryDownload = get("visible-summary-download");
  const summaryDownloadStatus = get("visible-summary-download-status");
  const sourceNames = [get("report-run-a").textContent, get("report-run-b").textContent];
  const svgNamespace = "http://www.w3.org/2000/svg";
  const metrics = [
    {label: "RTT", index: 2, unit: "ms", colors: ["#245ac4", "#527dcf"]},
    {label: "Up loss", legend: "Up", index: 3, unit: "%", colors: ["#00695e", "#228c7e"]},
    {label: "Down loss", legend: "Down", index: 4, unit: "%", colors: ["#a3440b", "#c76828"]},
    {label: "Probe loss", legend: "Probe", index: 5, unit: "%", colors: ["#9b2860", "#bf548b"]},
    {label: "Up traffic", legend: "Up", index: 6, unit: "KiB/s", colors: ["#00695e", "#228c7e"]},
    {label: "Down traffic", legend: "Down", index: 7, unit: "KiB/s", colors: ["#a3440b", "#c76828"]}
  ];
  const charts = [
    {svg: get("chart-rtt"), name: "RTT (ms)", metrics: metrics.slice(0, 1), floor: 50},
    {svg: get("chart-loss"), name: "Loss (%)", metrics: metrics.slice(1, 4), floor: 100, fixed: true},
    {svg: get("chart-traffic"), name: "UDP traffic (KiB/s)", metrics: metrics.slice(4), floor: 1}
  ];
  const summaryMetrics = [
    {id: "mean_rtt", label: "Mean RTT (ms)", index: 2, unit: "ms"},
    {id: "p95_rtt", label: "p95 RTT (ms)", percentile: true, unit: "ms"},
    {id: "mean_uplink_loss", label: "Mean up loss", index: 3, unit: "%", deltaUnit: "pp"},
    {id: "mean_downlink_loss", label: "Mean down loss", index: 4, unit: "%", deltaUnit: "pp"},
    {id: "mean_probe_loss", label: "Mean probe loss", index: 5, unit: "%", deltaUnit: "pp"},
    {id: "mean_upload_kib_per_second", label: "Mean upload (KiB/s)", index: 6, unit: "KiB/s"},
    {id: "mean_download_kib_per_second", label: "Mean download (KiB/s)", index: 7, unit: "KiB/s"},
    {id: "mean_upload_packets_per_second", label: "Mean upload (pkt/s)", index: 8, unit: "pkt/s"},
    {id: "mean_download_packets_per_second", label: "Mean download (pkt/s)", index: 9, unit: "pkt/s"}
  ];
  const height = 220;
  const top = 47;
  const bottom = 186;
  const left = 64;
  const resolution = 1n << 52n;
  const colorGrid = "#dce3ed";
  const colorAxis = "#455771";
  const clamp = (value, min, max) => value < min ? min : value > max ? max : value;

  function svgNode(parent, tag, attributes, text) {
    const node = document.createElementNS(svgNamespace, tag);
    for (const [key, value] of Object.entries(attributes)) node.setAttribute(key, value);
    if (text !== undefined) node.textContent = text;
    parent.append(node);
    return node;
  }

  function elapsed(time, precision = 3) {
    const seconds = String(time / 1000n % 60n).padStart(2, "0");
    const fraction = String(time % 1000n).padStart(3, "0").slice(0, precision);
    return `${time / 60000n}:${seconds}${precision ? "." + fraction : ""}`;
  }

  function formatValue(value, digits) {
    return Math.abs(value) >= 1e6 ? value.toExponential(2) : value.toFixed(digits);
  }

  function lowerBound(rows, time) {
    let first = 0;
    let last = rows.length;
    while (first < last) {
      const middle = first + Math.floor((last - first) / 2);
      if (rows[middle][0] < time) first = middle + 1;
      else last = middle;
    }
    return first;
  }

  function nearest(rows, time) {
    const index = lowerBound(rows, time);
    const before = rows[index - 1];
    const after = rows[index];
    const beforeDistance = before ? time - before[0] : 751n;
    const afterDistance = after ? after[0] - time : 751n;
    if (beforeDistance <= afterDistance) return beforeDistance <= 750n ? before : undefined;
    return afterDistance <= 750n ? after : undefined;
  }

  const raw = JSON.parse(get("chart-data").textContent);
  const start = BigInt(raw.start);
  const end = BigInt(raw.end);
  const fullSpan = end - start;
  const minSpan = fullSpan < 250n ? fullSpan : 250n;
  const clients = new Map(raw.clients.map(client => [String(client.id), client.runs.map(run =>
    run.map(row => [BigInt(row[0]), ...row.slice(1)])
  )]));
  if (!clients.size || fullSpan < 0n) return;
  for (const id of clients.keys()) {
    const option = document.createElement("option");
    option.value = id;
    option.textContent = "Client " + id;
    select.append(option);
  }

  let from = start;
  let to = end;
  let cursor;
  let drag;
  let visibleRuns = [[], []];
  let summarySnapshot;

  function xPosition(chart, time) {
    return from === to ? (left + chart.right) / 2
      : left + Number(time - from) / Number(to - from) * (chart.right - left);
  }

  function pointerTime(chart, event, clampOutside) {
    const box = chart.svg.getBoundingClientRect();
    if (!box.width || !box.height) return undefined;
    const x = (event.clientX - box.left) * chart.width / box.width;
    const y = (event.clientY - box.top) * height / box.height;
    if (!clampOutside && (x < left || x > chart.right || y < top || y > bottom)) return undefined;
    const ratio = clamp((x - left) / (chart.right - left), 0, 1);
    return from + ((to - from) * BigInt(Math.round(ratio * Number(resolution))) + resolution / 2n) / resolution;
  }

  function ceiling(chart) {
    let max = chart.floor;
    if (chart.fixed) return max;
    for (const run of visibleRuns) {
      for (const row of run) {
        for (const metric of chart.metrics) {
          if (row[metric.index] !== null) max = Math.max(max, row[metric.index]);
        }
      }
    }
    const power = 10 ** Math.floor(Math.log10(max));
    return power * ([1, 2, 5, 10].find(step => max / power <= step) || 10);
  }

  function plotMetric(chart, run, metric, runIndex, max) {
    const color = metric.colors[runIndex];
    let path = "";
    let segment = [];
    const finish = () => {
      if (segment.length === 1) {
        svgNode(chart.svg, "circle", {cx: segment[0][0], cy: segment[0][1], r: 3, fill: runIndex ? "#fff" : color, stroke: color});
      } else if (segment.length) {
        path += segment.map(([x, y], index) => `${index ? "L" : "M"}${x.toFixed(2)} ${y.toFixed(2)}`).join(" ");
      }
      segment = [];
    };
    for (const row of run) {
      const value = row[metric.index];
      if (row[1] || value === null) finish();
      if (value !== null) {
        segment.push([xPosition(chart, row[0]), bottom - clamp(value / max, 0, 1) * (bottom - top)]);
      }
    }
    finish();
    if (path) {
      const line = svgNode(chart.svg, "path", {d: path, fill: "none", stroke: color, "stroke-width": 2,
        "stroke-linejoin": "round", "vector-effect": "non-scaling-stroke"});
      if (runIndex) line.setAttribute("stroke-dasharray", "5 4");
    }
  }

  function renderChart(chart) {
    const {svg} = chart;
    chart.width = Math.max(240, Math.round(svg.getBoundingClientRect().width || 900));
    chart.right = chart.width - 12;
    svg.setAttribute("viewBox", `0 0 ${chart.width} ${height}`);
    svg.replaceChildren();
    svgNode(svg, "text", {x: 2, y: 16, fill: "#182230", "font-size": 15, "font-weight": 650}, chart.name);
    chart.metrics.forEach((metric, index) => {
      if (!metric.legend) return;
      const x = left + index * 61;
      svgNode(svg, "line", {x1: x, x2: x + 13, y1: 32, y2: 32, stroke: metric.colors[0], "stroke-width": 2});
      svgNode(svg, "text", {x: x + 17, y: 36, fill: colorAxis, "font-size": 11}, metric.legend);
    });
    svgNode(svg, "rect", {x: left, y: top, width: chart.right - left, height: bottom - top, fill: "#f8fafc"});
    const max = ceiling(chart);
    for (let i = 0; i <= 4; ++i) {
      const y = bottom - (bottom - top) * i / 4;
      svgNode(svg, "line", {x1: left, x2: chart.right, y1: y, y2: y, stroke: colorGrid});
      svgNode(svg, "text", {x: left - 8, y: y + 4, "text-anchor": "end", fill: colorAxis, "font-size": 11},
        formatValue(max * i / 4, max >= 100 ? 0 : 2));
    }
    const span = to - from;
    const tickCount = span === 0n ? 0 : chart.width > 550 ? 4 : 2;
    const precision = span < 1000n ? 3 : span < 10000n ? 1 : 0;
    for (let i = 0; i <= tickCount; ++i) {
      const time = tickCount ? from + span * BigInt(i) / BigInt(tickCount) : from;
      const x = xPosition(chart, time);
      const anchor = !tickCount ? "middle" : i === 0 ? "start" : i === tickCount ? "end" : "middle";
      svgNode(svg, "line", {x1: x, x2: x, y1: top, y2: bottom, stroke: colorGrid});
      svgNode(svg, "text", {x, y: 205, "text-anchor": anchor, fill: colorAxis, "font-size": 11}, elapsed(time, precision));
    }
    visibleRuns.forEach((run, runIndex) => chart.metrics.forEach(metric => plotMetric(chart, run, metric, runIndex, max)));
    chart.selection = svgNode(svg, "rect", {y: top, height: bottom - top, fill: "#245ac4", opacity: 0.14, visibility: "hidden"});
    chart.crosshair = svgNode(svg, "line", {y1: top, y2: bottom, stroke: "#243550", visibility: "hidden"});
  }

  const cell = (tag, value) => {
    const node = document.createElement(tag);
    node.textContent = value;
    return node;
  };
  const heading = cell("p", "");
  const readout = document.createElement("table");
  readout.className = "chart-values";
  readout.setAttribute("aria-label", "Comparison values at the cursor");
  const head = readout.createTHead().insertRow();
  const columnHeaders = ["Measurement", "Run A", "Run B"].map(label => cell("th", label));
  for (const header of columnHeaders) header.scope = "col";
  head.append(...columnHeaders);
  const rows = readout.createTBody();
  const valueCells = metrics.map(metric => {
    const row = rows.insertRow();
    const label = cell("th", metric.label);
    label.scope = "row";
    row.append(label);
    return [row.insertCell(), row.insertCell()];
  });
  tooltip.replaceChildren(heading, readout);

)JS", R"JS(
  function summarize(run) {
    const means = Array(10).fill(0);
    const counts = Array(10).fill(0);
    const rtts = [];
    for (const row of run) {
      for (let index = 2; index < means.length; ++index) {
        const value = row[index];
        if (!Number.isFinite(value) || value < 0) continue;
        means[index] += (value - means[index]) / ++counts[index];
        if (index === 2) rtts.push(value);
      }
    }
    rtts.sort((a, b) => a - b);
    const p95 = rtts[rtts.length - Math.floor(rtts.length / 20) - 1];
    return summaryMetrics.map(metric => metric.percentile ? p95 : counts[metric.index] ? means[metric.index] : undefined);
  }

  function summaryValue(value, unit, delta = false) {
    if (value === undefined) {
      const missing = cell("td", "—");
      missing.className = "missing";
      missing.title = "No data";
      return missing;
    }
    const shown = Math.abs(value) < 0.005 ? 0 : value;
    const suffix = unit === "%" ? "%" : unit === "pp" ? " pp" : "";
    return cell("td", (delta && shown > 0 ? "+" : "") + formatValue(shown, 2) + suffix);
  }

  function updateSummary() {
    const client = select.value;
    if (summarySnapshot?.client === client && summarySnapshot.from === from && summarySnapshot.to === to) return;

    const [a, b] = visibleRuns.map(summarize);
    const values = summaryMetrics.map((metric, index) => ({
      metric,
      a: a[index],
      b: b[index],
      delta: a[index] === undefined || b[index] === undefined ? undefined : b[index] - a[index]
    }));
    const rows = values.map(({metric, a, b, delta}) => {
      const row = document.createElement("tr");
      const label = cell("th", metric.label);
      label.scope = "row";
      row.append(label, summaryValue(a, metric.unit), summaryValue(b, metric.unit),
        summaryValue(delta, metric.deltaUnit ?? metric.unit, true));
      return row;
    });
    summaryWindow.textContent = `Client ${client} · ${windowLabel.textContent}`;
    summaryRows.replaceChildren(...rows);
    summarySnapshot = {client, from, to, values};
    summaryDownloadStatus.textContent = "";
  }

  function csvText(text) {
    const formulaPrefix = /^(?:[\t\r\n]|[ \t\r\n]*[=+@-])/.test(text) ? "'" : "";
    return '"' + formulaPrefix + text.replaceAll('"', '""') + '"';
  }

  function downloadSummaryCsv() {
    if (!summarySnapshot) return;
    const {client, from, to, values} = summarySnapshot;
    try {
      const lines = ["client_index,range,range_start_ms,range_end_ms,run_a_file,run_b_file,metric,unit,run_a,run_b,delta_b_minus_a,delta_unit"];
      const prefix = [client, csvText("Visible range"), from, to, ...sourceNames.map(csvText)].join(",");
      for (const {metric, a, b, delta} of values) {
        lines.push([prefix, metric.id, metric.unit, a, b, delta, metric.deltaUnit ?? metric.unit].join(","));
      }
      const blob = new Blob([lines.join("\r\n") + "\r\n"], {type: "text/csv;charset=utf-8"});
      const link = document.createElement("a");
      link.download = `network-comparison-client-${client}-${from}-${to}.csv`;
      const url = URL.createObjectURL(blob);
      try {
        link.href = url;
        link.hidden = true;
        document.body.append(link);
        link.click();
        summaryDownloadStatus.textContent = "CSV download requested.";
      } finally {
        link.remove();
        window.setTimeout(() => URL.revokeObjectURL(url), 1000);
      }
    } catch {
      summaryDownloadStatus.textContent = "Could not download the CSV.";
    }
  }

  function updateCursor() {
    for (const chart of charts) {
      chart.crosshair.setAttribute("visibility", cursor === undefined ? "hidden" : "visible");
      if (cursor !== undefined) {
        const x = xPosition(chart, cursor);
        chart.crosshair.setAttribute("x1", x);
        chart.crosshair.setAttribute("x2", x);
      }
      chart.selection.setAttribute("visibility", drag ? "visible" : "hidden");
      if (drag) {
        const a = xPosition(chart, drag.start);
        const b = xPosition(chart, drag.end);
        chart.selection.setAttribute("x", Math.min(a, b));
        chart.selection.setAttribute("width", Math.abs(a - b));
      }
    }
    readout.hidden = cursor === undefined;
    heading.textContent = cursor === undefined
      ? "Point to a chart to compare the nearest samples."
      : "Cursor at " + elapsed(cursor);
    if (cursor === undefined) return;

    const samples = visibleRuns.map(run => nearest(run, cursor));
    samples.forEach((row, index) => {
      const name = index ? "Run B" : "Run A";
      columnHeaders[index + 1].textContent = name + (row ? " · " + elapsed(row[0]) : " · No sample");
    });
    metrics.forEach((metric, index) => {
      samples.forEach((row, run) => {
        const value = row?.[metric.index];
        valueCells[index][run].textContent = value == null ? "—"
          : formatValue(value, metric.index === 2 ? 0 : 2) + (metric.unit === "%" ? "" : " ") + metric.unit;
      });
    });
  }

  function draw() {
    visibleRuns = clients.get(select.value).map(run => {
      const first = lowerBound(run, from);
      return run.slice(first, lowerBound(run, to + 1n));
    });
    charts.forEach(renderChart);
    windowLabel.textContent = `${elapsed(from)} – ${elapsed(to)} elapsed`;
    updateSummary();
    const missing = visibleRuns.flatMap((run, index) => run.length ? [] : [index ? "Run B" : "Run A"]);
    status.textContent = missing.length ? "No samples in view for " + missing.join(" and ") + "." : "";
    const span = to - from;
    zoomIn.disabled = span <= minSpan;
    zoomOut.disabled = span >= fullSpan;
    reset.disabled = from === start && to === end;
    updateCursor();
  }

  function setWindow(first, last) {
    const span = clamp(last - first, minSpan, fullSpan);
    from = clamp(first, start, end - span);
    to = from + span;
    cursor = undefined;
    draw();
  }

  function zoom(inwards) {
    const span = to - from;
    if ((inwards && span <= minSpan) || (!inwards && span >= fullSpan)) return;
    const next = clamp(inwards ? span / 2n : span * 2n, minSpan, fullSpan);
    const anchor = cursor ?? from + span / 2n;
    const first = anchor - (anchor - from) * next / span;
    setWindow(first, first + next);
  }

  for (const chart of charts) {
    const {svg} = chart;
    svg.setAttribute("tabindex", "0");
    svg.setAttribute("aria-label", chart.name + ". Use arrow keys to inspect, plus and minus to zoom, and Home to reset.");
    svg.addEventListener("pointerdown", event => {
      if (!event.isPrimary || event.button !== 0 || drag) return;
      const time = pointerTime(chart, event, false);
      if (time === undefined) return;
      svg.setPointerCapture(event.pointerId);
      cursor = time;
      drag = {chart, id: event.pointerId, x: event.clientX, start: time, end: time};
      updateCursor();
    });
    svg.addEventListener("pointermove", event => {
      if (drag && drag.id !== event.pointerId) return;
      cursor = pointerTime(chart, event, Boolean(drag));
      if (drag && cursor !== undefined) drag.end = cursor;
      updateCursor();
    });
    svg.addEventListener("pointerup", event => {
      if (!drag || drag.id !== event.pointerId) return;
      const first = drag.start;
      const last = pointerTime(chart, event, true) ?? drag.end;
      const distance = Math.abs(event.clientX - drag.x);
      drag = undefined;
      if (distance >= 5 && first !== last && to - from > minSpan) {
        const begin = first < last ? first : last;
        const end = first < last ? last : first;
        const span = end - begin;
        setWindow(span < minSpan ? begin - (minSpan - span) / 2n : begin,
          span < minSpan ? end + (minSpan - span + 1n) / 2n : end);
      } else {
        cursor = last;
        updateCursor();
      }
    });
    const clearCursor = () => {
      cursor = undefined;
      updateCursor();
    };
    svg.addEventListener("pointerleave", event => {
      if (!drag && event.pointerType !== "touch") clearCursor();
    });
    svg.addEventListener("lostpointercapture", () => {
      if (!drag || drag.chart !== chart) return;
      drag = undefined;
      clearCursor();
    });
    svg.addEventListener("keydown", event => {
      if (event.key === "ArrowLeft" || event.key === "ArrowRight") {
        const position = cursor ?? from + (to - from) / 2n;
        const step = (to - from) / 60n || 1n;
        cursor = clamp(event.key === "ArrowLeft" ? position - step : position + step, from, to);
        updateCursor();
      } else if (event.key === "+" || event.key === "=") zoom(true);
      else if (event.key === "-") zoom(false);
      else if (event.key === "Home") setWindow(start, end);
      else if (event.key === "Escape") clearCursor();
      else return;
      event.preventDefault();
    });
  }

  select.addEventListener("change", () => {
    cursor = undefined;
    draw();
  });
  zoomIn.addEventListener("click", () => zoom(true));
  zoomOut.addEventListener("click", () => zoom(false));
  reset.addEventListener("click", () => setWindow(start, end));
  summaryDownload.addEventListener("click", downloadSummaryCsv);
  document.querySelectorAll("[data-chart-client]").forEach(link => {
    link.addEventListener("click", event => {
      if (event.altKey || event.ctrlKey || event.metaKey || event.shiftKey || !clients.has(link.dataset.chartClient)) return;
      event.preventDefault();
      select.value = link.dataset.chartClient;
      cursor = undefined;
      draw();
      panel.scrollIntoView();
      select.focus({preventScroll: true});
    });
  });

  panel.hidden = false;
  try {
    draw();
  } catch (error) {
    panel.hidden = true;
    throw error;
  }
  fallback.open = false;
  summary.hidden = false;
  window.addEventListener("resize", () => {
    if (charts.some(chart => chart.width !== Math.max(240, Math.round(chart.svg.getBoundingClientRect().width || 900)))) draw();
  });
})();
)JS"};

}
