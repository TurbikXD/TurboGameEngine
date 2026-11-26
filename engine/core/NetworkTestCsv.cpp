#include "engine/core/NetworkTestCsv.h"
#include "engine/core/AtomicFile.h"
#include "engine/core/NetworkTestReportHtml.h"
#include "engine/core/StoredZip.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace engine::core {
namespace {
constexpr std::size_t MaxCsvBytes = 16U * 1024U * 1024U;
constexpr std::size_t ColumnCount = 14;
constexpr double MaxTrafficRate = static_cast<double>(std::numeric_limits<std::uint64_t>::max()) * 1'000.0;
constexpr std::string_view PngSignature = "\x89PNG\r\n\x1a\n";
using Columns = std::array<std::string_view, ColumnCount>;

struct CsvSample {
    net::NetworkTestHistoryPoint point;
    std::size_t line;
};

std::string readContents(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("Select a Network Test CSV file.");
    const auto size = std::filesystem::file_size(path);
    if (size > MaxCsvBytes) throw std::runtime_error("Network Test CSV exceeds 16 MiB.");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open the Network Test CSV.");

    std::string contents;
    contents.reserve(static_cast<std::size_t>(size));
    std::array<char, 32'768> buffer;
    for (;;) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count > MaxCsvBytes - contents.size()) throw std::runtime_error("Network Test CSV exceeds 16 MiB.");
        contents.append(buffer.data(), count);
        if (input.bad() || (input.fail() && !input.eof())) throw std::runtime_error("Could not read the Network Test CSV.");
        if (input.eof()) return contents;
    }
}

std::string_view trim(std::string_view field) {
    constexpr std::string_view whitespace = " \t\r";
    const auto first = field.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    return field.substr(first, field.find_last_not_of(whitespace) - first + 1);
}

bool splitColumns(std::string_view line, Columns& columns) {
    for (std::size_t index = 0; index < columns.size(); ++index) {
        const auto comma = line.find(',');
        const bool last = index + 1 == columns.size();
        if ((comma == std::string_view::npos) != last) return false;
        auto field = trim(line.substr(0, comma));
        if (field.size() >= 2 && field.front() == '"' && field.back() == '"') {
            field = trim(field.substr(1, field.size() - 2));
        }
        if (field.find('"') != std::string_view::npos) return false;
        columns[index] = field;
        if (!last) line.remove_prefix(comma + 1);
    }
    return true;
}

template <typename Number>
bool number(std::string_view field, Number& value) {
    if (field.empty()) return false;
    const auto end = field.data() + field.size();
    const auto parsed = std::from_chars(field.data(), end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

template <typename Number>
bool optionalNumber(std::string_view field, std::optional<Number>& value, Number maximum) {
    if (field.empty()) return true;
    Number parsed{};
    if (!number(field, parsed) || !std::isfinite(parsed) || parsed < Number{} || parsed > maximum) return false;
    value = parsed;
    return true;
}

bool traffic(std::string_view bytes, std::string_view packets, std::optional<net::NetworkTestTrafficRate>& rate) {
    if (bytes.empty() && packets.empty()) return true;
    net::NetworkTestTrafficRate sample;
    if (!number(bytes, sample.bytesPerSecond) || !number(packets, sample.packetsPerSecond) ||
        !std::isfinite(sample.bytesPerSecond) || !std::isfinite(sample.packetsPerSecond) ||
        sample.bytesPerSecond < 0.0 || sample.bytesPerSecond > MaxTrafficRate ||
        sample.packetsPerSecond < 0.0 || sample.packetsPerSecond > MaxTrafficRate) return false;
    rate = sample;
    return true;
}

[[noreturn]] void invalidRow(std::size_t line, std::string_view reason) {
    throw std::runtime_error("Line " + std::to_string(line) + ": " + std::string(reason));
}

void parseSample(const Columns& columns, unsigned& client, CsvSample& sample) {
    auto& point = sample.point;
    auto& metrics = point.metrics;
    unsigned breakBefore{};
    if (!number(columns[0], client) || client == 0 || client > static_cast<unsigned>(net::MaxNetworkTestClients))
        invalidRow(sample.line, "Invalid client index.");
    if (!number(columns[1], point.elapsedMs)) invalidRow(sample.line, "Invalid elapsed time.");
    if (!number(columns[2], breakBefore) || breakBefore > 1) invalidRow(sample.line, "Invalid chart segment marker.");
    point.breakBefore = breakBefore != 0;
    if (!number(columns[3], point.profile.revision) || point.profile.revision == 0 ||
        !number(columns[4], point.profile.conditions.latencyMs) || !number(columns[5], point.profile.conditions.lossPercent) ||
        !point.profile.conditions.valid()) invalidRow(sample.line, "Invalid network profile.");
    if (!optionalNumber(columns[6], metrics.rttMs, std::numeric_limits<std::uint32_t>::max()))
        invalidRow(sample.line, "Invalid RTT measurement.");
    const auto maxLossPercent = std::nextafter(100.0, std::numeric_limits<double>::infinity());
    if (!optionalNumber(columns[7], metrics.uplinkLossPercent, maxLossPercent) ||
        !optionalNumber(columns[8], metrics.downlinkLossPercent, maxLossPercent) ||
        !optionalNumber(columns[9], metrics.probeLossPercent, maxLossPercent))
        invalidRow(sample.line, "Invalid packet loss measurement.");
    if (!traffic(columns[10], columns[11], metrics.upload) || !traffic(columns[12], columns[13], metrics.download))
        invalidRow(sample.line, "Invalid UDP traffic measurement.");
}

bool sameFile(const std::filesystem::path& target, const std::filesystem::path& source) {
    if (std::filesystem::weakly_canonical(target) == std::filesystem::weakly_canonical(source)) return true;
    return std::filesystem::exists(target) && std::filesystem::exists(source) && std::filesystem::equivalent(target, source);
}

void writeCsvText(std::ostream& output, std::string_view field) {
    output << '"';
    const auto first = field.find_first_not_of(" \t\r\n");
    if ((first != std::string_view::npos && std::string_view("=+-@").find(field[first]) != std::string_view::npos) ||
        (!field.empty() && std::string_view("\t\r\n").find(field.front()) != std::string_view::npos)) output << '\'';
    for (const char c : field) {
        if (c == '"') output << '"';
        output << c;
    }
    output << '"';
}

void writeCsvFilename(std::ostream& output, const std::filesystem::path& path) {
    const auto name = path.filename().u8string();
    writeCsvText(output, {reinterpret_cast<const char*>(name.data()), name.size()});
}

void writeSummaryValue(std::ostream& output, std::optional<double> value) {
    if (value) output << (*value == 0.0 ? 0.0 : *value);
}

bool validSummary(const NetworkTestSummaryCsv& summary) {
    return summary.clientIndex != 0 && summary.clientIndex <= static_cast<unsigned>(net::MaxNetworkTestClients) &&
        summary.rangeStartMs <= summary.rangeEndMs && !summary.rangeLabel.empty() &&
        !summary.runAPath.filename().empty() && !summary.runBPath.filename().empty();
}

bool hasExtension(const std::filesystem::path& path, std::u8string_view expected) {
    const auto extension = path.extension().u8string();
    return std::ranges::equal(extension, expected, [](char8_t actual, char8_t lower) {
        return actual == lower || (actual >= u8'A' && actual <= u8'Z' && actual + (u8'a' - u8'A') == lower);
    });
}

constexpr std::string_view SummaryHeader =
    "client_index,range,range_start_ms,range_end_ms,run_a_file,run_b_file,metric,unit,run_a,run_b,"
    "delta_b_minus_a,delta_unit\r\n";

std::string serializeSummary(const NetworkTestSummaryCsv& summary) {
    std::ostringstream metadata;
    metadata.exceptions(std::ios::badbit | std::ios::failbit);
    metadata.imbue(std::locale::classic());
    metadata << summary.clientIndex << ',';
    writeCsvText(metadata, summary.rangeLabel);
    metadata << ',' << summary.rangeStartMs << ',' << summary.rangeEndMs << ',';
    writeCsvFilename(metadata, summary.runAPath);
    metadata << ',';
    writeCsvFilename(metadata, summary.runBPath);
    metadata << ',';
    const auto prefix = std::move(metadata).str();

    std::ostringstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << SummaryHeader;
    for (const auto& metric : net::NetworkTestSummaryMetrics) {
        const auto values = metric.values(summary.runA, summary.runB);
        output << prefix << metric.id << ',' << metric.unit << ',';
        writeSummaryValue(output, values.runA);
        output << ',';
        writeSummaryValue(output, values.runB);
        output << ',';
        writeSummaryValue(output, values.delta);
        output << ',' << metric.deltaUnit << "\r\n";
    }
    return std::move(output).str();
}

std::vector<NewFileContents> prepareComparisonReportFiles(const std::filesystem::path& baseStem,
                                                        std::span<const NetworkTestComparisonReport> reports) {
    if (reports.empty() || reports.size() > static_cast<std::size_t>(net::MaxNetworkTestClients)) {
        throw std::invalid_argument("Invalid number of clients for the comparison report.");
    }

    const auto& selection = reports.front().summary;
    std::array<std::size_t, net::MaxNetworkTestClients + 1> csvIndexByClient{};
    std::vector<NewFileContents> files;
    files.reserve(reports.size() * 2 + 1);
    for (const auto& report : reports) {
        const auto& summary = report.summary;
        if (!validSummary(summary) || csvIndexByClient[summary.clientIndex] != 0 || summary.rangeLabel != selection.rangeLabel ||
            summary.rangeStartMs != selection.rangeStartMs || summary.rangeEndMs != selection.rangeEndMs ||
            summary.runAPath != selection.runAPath || summary.runBPath != selection.runBPath) {
            throw std::invalid_argument("Invalid Network Test batch report selection.");
        }
        if (!report.pngBytes.starts_with(PngSignature)) {
            throw std::invalid_argument("Invalid report chart image for Client " + std::to_string(summary.clientIndex) + ".");
        }

        auto clientStem = baseStem;
        clientStem += "-client-" + std::to_string(summary.clientIndex);
        auto pngPath = clientStem;
        pngPath += ".png";
        clientStem += ".csv";
        files.push_back({std::move(pngPath), {}});
        csvIndexByClient[summary.clientIndex] = files.size();
        files.push_back({std::move(clientStem), serializeSummary(summary)});
    }

    auto summaryPath = baseStem;
    summaryPath += "-summary.csv";
    std::string combinedSummary(SummaryHeader);
    for (const auto csvIndex : csvIndexByClient) {
        if (csvIndex != 0) combinedSummary.append(files[csvIndex].contents, SummaryHeader.size());
    }
    files.push_back({std::move(summaryPath), std::move(combinedSummary)});
    return files;
}

void moveReportImages(std::span<NewFileContents> files, std::span<NetworkTestComparisonReport> reports) {
    for (std::size_t index = 0; index < reports.size(); ++index) {
        files[index * 2].contents = std::move(reports[index].pngBytes);
    }
}
}

std::optional<NetworkTestCsvRun> readNetworkTestCsv(const std::filesystem::path& path, std::string& error) {
    error.clear();
    try {
        const auto contents = readContents(path);
        std::string_view lines = contents;
        if (lines.starts_with("\xef\xbb\xbf")) lines.remove_prefix(3);

        Columns header{};
        splitColumns(NetworkTestCsvHeader, header);
        bool foundHeader = false;
        std::map<unsigned, std::vector<CsvSample>> samples;
        for (std::size_t lineNumber = 1; !lines.empty(); ++lineNumber) {
            const auto newline = lines.find('\n');
            const auto line = trim(lines.substr(0, newline));
            lines.remove_prefix(newline == std::string_view::npos ? lines.size() : newline + 1);
            if (line.empty()) continue;

            Columns columns{};
            if (!splitColumns(line, columns)) invalidRow(lineNumber, "Expected 14 CSV fields.");
            if (!foundHeader) {
                if (columns != header) invalidRow(lineNumber, "Expected a Net Stats CSV header.");
                foundHeader = true;
                continue;
            }

            unsigned client{};
            CsvSample sample{{}, lineNumber};
            parseSample(columns, client, sample);
            auto& clientSamples = samples[client];
            if (clientSamples.size() >= net::MaxNetworkTestHistoryPoints)
                invalidRow(lineNumber, "Too many samples for this client.");
            clientSamples.push_back(std::move(sample));
        }
        if (samples.empty()) throw std::runtime_error("The Network Test CSV contains no samples.");

        NetworkTestCsvRun result;
        for (auto& [client, clientSamples] : samples) {
            std::ranges::sort(clientSamples, {}, [](const auto& sample) { return sample.point.elapsedMs; });
            if (clientSamples.back().point.elapsedMs - clientSamples.front().point.elapsedMs > net::NetworkTestHistoryWindowMs)
                invalidRow(clientSamples.back().line, "Client samples exceed the five-minute history window.");
            auto& history = result.clients[client];
            for (auto& sample : clientSamples) {
                if (!history.points().empty() && history.points().back().elapsedMs == sample.point.elapsedMs)
                    invalidRow(sample.line, "Duplicate elapsed time for this client.");
                history.append(std::move(sample.point));
            }
        }
        return result;
    } catch (const std::exception& failure) {
        error = failure.what();
        return std::nullopt;
    }
}

bool isNetworkTestCsvSource(const std::filesystem::path& target, const std::filesystem::path& runA,
                            const std::filesystem::path& runB) {
    return sameFile(target, runA) || sameFile(target, runB);
}

bool writeNetworkTestSummaryCsv(const std::filesystem::path& target, const NetworkTestSummaryCsv& summary, std::string& error) {
    error.clear();
    try {
        if (!validSummary(summary)) {
            error = "Invalid Network Test summary selection.";
            return false;
        }
        if (target.empty() || target.filename().empty()) {
            error = "File target is empty.";
            return false;
        }
        if (isNetworkTestCsvSource(target, summary.runAPath, summary.runBPath)) {
            error = "Choose a different file to keep both source CSVs.";
            return false;
        }

        return writeFileAtomically(target, serializeSummary(summary), error);
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

bool writeNetworkTestComparisonReport(const std::filesystem::path& pngPath, const std::string& pngBytes,
                                      const NetworkTestSummaryCsv& summary, std::string& error) {
    error.clear();
    try {
        if (!validSummary(summary)) {
            error = "Invalid Network Test summary selection.";
            return false;
        }
        if (!hasExtension(pngPath, u8".png")) {
            error = "Choose a report filename with a .png extension.";
            return false;
        }
        if (!pngBytes.starts_with(PngSignature)) {
            error = "Invalid report chart image.";
            return false;
        }
        auto csvPath = pngPath;
        csvPath.replace_extension(".csv");
        if (isNetworkTestCsvSource(pngPath, summary.runAPath, summary.runBPath) ||
            isNetworkTestCsvSource(csvPath, summary.runAPath, summary.runBPath)) {
            error = "Choose a different file to keep both source CSVs.";
            return false;
        }
        return writeNewFilePair(pngPath, pngBytes, csvPath, serializeSummary(summary), error);
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

bool writeNetworkTestComparisonReports(const std::filesystem::path& basePngPath,
                                       std::span<NetworkTestComparisonReport> reports, std::string& error) {
    error.clear();
    try {
        if (!hasExtension(basePngPath, u8".png")) {
            error = "Choose a report filename with a .png extension.";
            return false;
        }

        auto files = prepareComparisonReportFiles(basePngPath.parent_path() / basePngPath.stem(), reports);
        const auto& selection = reports.front().summary;
        if (std::ranges::any_of(files, [&](const auto& file) {
            return isNetworkTestCsvSource(file.path, selection.runAPath, selection.runBPath);
        })) {
            error = "Choose a different report name to keep both source CSVs.";
            return false;
        }
        moveReportImages(files, reports);
        return writeNewFiles(files, error);
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

bool writeNetworkTestComparisonArchive(const std::filesystem::path& zipPath,
                                       std::span<NetworkTestComparisonReport> reports, std::string& error) {
    error.clear();
    try {
        if (!hasExtension(zipPath, u8".zip")) {
            error = "Choose a report filename with a .zip extension.";
            return false;
        }
        auto files = prepareComparisonReportFiles(zipPath.stem(), reports);
        const auto& selection = reports.front().summary;
        if (isNetworkTestCsvSource(zipPath, selection.runAPath, selection.runBPath)) {
            error = "Choose a different report name to keep both source CSVs.";
            return false;
        }
        files.push_back({"index.html", renderNetworkTestReportHtml(reports)});
        moveReportImages(files, reports);
        const NewFileContents archive{zipPath, encodeStoredZip(files)};
        return writeNewFiles(std::span(&archive, 1), error);
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

}
