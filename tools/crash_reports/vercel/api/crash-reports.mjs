import { createHash } from "node:crypto";
import { crc32 } from "node:zlib";
import * as blob from "@vercel/blob";
import { fromBufferPromise } from "yauzl";

const maxPostBytes = 4 * 1024 * 1024;
const maxArchiveBytes = 64 * 1024 * 1024;
const reportNames = new Set(["crash.dmp", "engine.log", "report.json"]);
const reportIdPattern = /^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i;
const buildIdPattern = /^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$/;
const digestPattern = /^[0-9a-f]{64}$/;
const zipType = "application/zip";
const privateZip = { access: "private", contentType: zipType, allowOverwrite: false };

class RejectedReport extends Error {
  constructor(status) {
    super();
    this.status = status;
  }
}

function reply(status, headers = {}) {
  return new Response(null, { status, headers: { "Cache-Control": "no-store", ...headers } });
}

function receipt(status, reportId) {
  return reply(status, { "X-TGE-Report-Id": reportId });
}

function validReportId(reportId) {
  return typeof reportId === "string" && reportId.length === 36 && reportIdPattern.test(reportId);
}

async function validateArchive(bytes) {
  try {
    const end = bytes.length - 22;
    if (end < 0 || bytes.readUInt32LE(end) !== 0x06054b50 || bytes.readUInt32LE(0) !== 0x04034b50 ||
        bytes.readUInt16LE(end + 20) !== 0) throw new Error("ZIP layout");
    const directorySize = bytes.readUInt32LE(end + 12);
    const directoryOffset = bytes.readUInt32LE(end + 16);
    if (directorySize > 4096 || directoryOffset + directorySize !== end) throw new Error("ZIP directory");

    const zip = await fromBufferPromise(bytes, { lazyEntries: true, strictFileNames: true });
    if (zip.entryCount !== reportNames.size) throw new Error("ZIP entry count");
    const files = new Map();
    const ranges = [];
    for await (const entry of zip.eachEntry()) {
      const name = entry.fileName;
      const size = entry.uncompressedSize;
      if (!reportNames.has(name) || files.has(name) || entry.compressionMethod !== 0 ||
          (entry.generalPurposeBitFlag & ~0x800) !== 0 || size !== entry.compressedSize || size === 0) {
        throw new Error("ZIP entry");
      }
      const local = await zip.readLocalFileHeaderPromise(entry);
      const dataEnd = local.fileDataStart + size;
      const data = bytes.subarray(local.fileDataStart, dataEnd);
      if (dataEnd > directoryOffset || !local.fileName.equals(Buffer.from(name)) ||
          local.compressionMethod !== 0 || local.generalPurposeBitFlag !== entry.generalPurposeBitFlag ||
          local.uncompressedSize !== size || local.compressedSize !== size ||
          local.crc32 !== entry.crc32 || crc32(data) !== entry.crc32) {
        throw new Error("ZIP entry data");
      }
      files.set(name, data);
      ranges.push([entry.relativeOffsetOfLocalHeader, dataEnd]);
    }
    ranges.sort(([a], [b]) => a - b);
    let offset = 0;
    for (const [start, finish] of ranges) {
      if (start !== offset) throw new Error("ZIP entry bounds");
      offset = finish;
    }
    if (offset !== directoryOffset) throw new Error("ZIP entry bounds");

    const dump = files.get("crash.dmp");
    if (dump.length < 32 || dump.toString("latin1", 0, 4) !== "MDMP" || dump.readUInt16LE(4) !== 0xa793) {
      throw new Error("Minidump header");
    }
    const streamCount = dump.readUInt32LE(8);
    const streamDirectory = dump.readUInt32LE(12);
    if (!streamCount || streamDirectory < 32 || streamDirectory + 12 * streamCount > dump.length) {
      throw new Error("Minidump stream directory");
    }
    const metadata = files.get("report.json");
    if (metadata.length > 16 * 1024) throw new Error("Metadata size");
    const { reportId, buildId, schemaVersion } = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(metadata));
    if (schemaVersion !== 1 || !validReportId(reportId) || typeof buildId !== "string" ||
        !buildIdPattern.test(buildId)) throw new Error("Report identity");
    return reportId;
  } catch {
    throw new RejectedReport(400);
  }
}

async function requestBytes(request, limit) {
  const length = request.headers.get("content-length");
  if (length === null) throw new RejectedReport(411);
  if (!/^[0-9]{1,20}$/.test(length)) throw new RejectedReport(400);
  const size = Number(length);
  if (size > limit) throw new RejectedReport(413);
  const bytes = Buffer.from(await request.arrayBuffer());
  if (bytes.length !== size || size === 0) throw new RejectedReport(400);
  return bytes;
}

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function reportPath(reportId) {
  return "reports/" + reportId.toLowerCase() + ".zip";
}

async function readBlob(storage, pathname) {
  const result = await storage.get(pathname, { access: "private", useCache: false });
  if (!result) return null;
  if (result.statusCode !== 200 || result.blob.size <= 0 || result.blob.size > maxArchiveBytes) {
    await result.stream?.cancel();
    throw new Error("Invalid stored blob");
  }
  const bytes = Buffer.from(await new Response(result.stream).arrayBuffer());
  if (bytes.length !== result.blob.size) throw new Error("Incomplete stored blob");
  return bytes;
}

async function saveReport(storage, reportId, bytes, pendingPath) {
  const pathname = reportPath(reportId);
  try {
    if (pendingPath) await storage.copy(pendingPath, pathname, privateZip);
    else await storage.put(pathname, bytes, privateZip);
  } catch (error) {
    const existing = await readBlob(storage, pathname);
    if (!existing) throw error;
    return existing.equals(bytes) ? receipt(200, reportId) : reply(409);
  }
  return receipt(201, reportId);
}

async function handleLargeReport(storage, request) {
  const body = await requestBytes(request, 512);
  let metadata;
  try {
    metadata = JSON.parse(body);
  } catch {
    throw new RejectedReport(400);
  }
  const { stage, reportId, size, sha256: digest } = metadata ?? {};
  if ((stage !== "prepare" && stage !== "complete") || !validReportId(reportId) ||
      !Number.isSafeInteger(size) || size <= maxPostBytes || size > maxArchiveBytes ||
      typeof digest !== "string" || digest.length !== 64 || !digestPattern.test(digest)) {
    throw new RejectedReport(400);
  }

  const matches = (bytes) => bytes.length === size && sha256(bytes) === digest;
  const existing = await readBlob(storage, reportPath(reportId));
  if (existing) return matches(existing) ? receipt(200, reportId) : reply(409);

  const pathname = "pending/" + reportId.toLowerCase() + "-" + digest + ".zip";
  const pending = await readBlob(storage, pathname);
  if (pending) {
    if (!matches(pending) || await validateArchive(pending) !== reportId) throw new RejectedReport(400);
    return saveReport(storage, reportId, pending, pathname);
  }
  if (stage === "complete") throw new RejectedReport(409);

  const validUntil = Date.now() + 5 * 60 * 1000;
  const limits = { validUntil, maximumSizeInBytes: size, allowedContentTypes: [zipType] };
  const token = await storage.issueSignedToken({ pathname, operations: ["put"], ...limits });
  const { presignedUrl } = await storage.presignUrl(token, {
    pathname, operation: "put", access: "private", allowOverwrite: false, ttlDays: 1, ...limits,
  });
  return reply(202, { "X-TGE-Upload-Url": presignedUrl });
}

export function createReceiver(storage) {
  return async (request) => {
    if (request.method !== "POST") return reply(405, { Allow: "POST" });
    if (new URL(request.url).search) return reply(400);

    try {
      const type = request.headers.get("content-type")?.split(";", 1)[0].trim().toLowerCase();
      if (type === "application/json") return await handleLargeReport(storage, request);
      if (type !== zipType) return reply(415);
      const bytes = await requestBytes(request, maxPostBytes);
      const reportId = await validateArchive(bytes);
      return await saveReport(storage, reportId, bytes);
    } catch (error) {
      if (error instanceof RejectedReport) return reply(error.status);
      console.error("Crash report storage request failed", error?.name);
      return reply(503);
    }
  };
}

export default { fetch: createReceiver(blob) };
