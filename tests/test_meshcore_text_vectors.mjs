import assert from "node:assert/strict";
import {
  createCipheriv,
  createDecipheriv,
  createHash,
  createHmac,
} from "node:crypto";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

// These vectors describe the smallest MeshCore v1.17.1 text slice that a
// future Kitsu transport can implement.  They do not touch a serial port or
// radio.  Keeping them independent of firmware code makes protocol drift
// visible before an on-air test.

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..");

const ROUTE_TYPE_FLOOD = 0x01;
const PAYLOAD_TYPE_TXT_MSG = 0x02;
const PAYLOAD_TYPE_GRP_TXT = 0x05;
const TXT_TYPE_PLAIN = 0;

const PUBLIC_PSK_16 = Buffer.from(
  "8B3387E9C5CDEA6AC9E5EDBAA115CD72",
  "hex",
);
const PUBLIC_SECRET_32 = Buffer.concat([PUBLIC_PSK_16, Buffer.alloc(16)]);

const GROUP_VECTOR = {
  timestamp: 1_700_000_000,
  text: "🦊 Kitsu KTDEAD: V09-GROUP-01234567",
  wireHex:
    "15001107CAB76B1C8186E1DA6D45DE5790A52E43E566FB7DE69C4595104B990F368DEA319022E3846604B6A49D82C7BE8426E491DD",
};

const DIRECT_VECTOR = {
  timestamp: 1_700_000_001,
  text: "V09-DIRECT-01234567",
  wireHex:
    "0900B2A1DC6186C04D6FA160B856A57AF8D7C81777CFA260EEB69FD903B42E6ED03FCF676845",
  ackHex: "9219858F",
};

function zeroPad16(input) {
  assert(input.length > 0, "MeshCore never encrypts an empty text payload");
  const output = Buffer.alloc(Math.ceil(input.length / 16) * 16);
  input.copy(output);
  return output;
}

function aes128EcbEncrypt(secret32, plaintext) {
  const cipher = createCipheriv("aes-128-ecb", secret32.subarray(0, 16), null);
  cipher.setAutoPadding(false);
  return Buffer.concat([cipher.update(zeroPad16(plaintext)), cipher.final()]);
}

function aes128EcbDecrypt(secret32, ciphertext) {
  assert.equal(ciphertext.length % 16, 0);
  const decipher = createDecipheriv(
    "aes-128-ecb",
    secret32.subarray(0, 16),
    null,
  );
  decipher.setAutoPadding(false);
  return Buffer.concat([decipher.update(ciphertext), decipher.final()]);
}

function mac2(secret32, ciphertext) {
  return createHmac("sha256", secret32).update(ciphertext).digest().subarray(0, 2);
}

function plainTextData(timestamp, text, attempt = 0) {
  const header = Buffer.alloc(5);
  header.writeUInt32LE(timestamp, 0);
  header[4] = (attempt & 0x03) | (TXT_TYPE_PLAIN << 2);
  return Buffer.concat([header, Buffer.from(text, "utf8")]);
}

function decodeZeroPaddedText(data) {
  const end = data.indexOf(0, 5);
  return data.subarray(5, end < 0 ? data.length : end).toString("utf8");
}

function buildGroupWire(timestamp, text) {
  const data = plainTextData(timestamp, text);
  const ciphertext = aes128EcbEncrypt(PUBLIC_SECRET_32, data);
  const channelHash = createHash("sha256").update(PUBLIC_PSK_16).digest()[0];
  const header = (PAYLOAD_TYPE_GRP_TXT << 2) | ROUTE_TYPE_FLOOD;
  return Buffer.concat([
    Buffer.from([header, 0x00, channelHash]),
    mac2(PUBLIC_SECRET_32, ciphertext),
    ciphertext,
  ]);
}

function buildDirectWire(timestamp, text, senderPublic, recipientPublic, secret32) {
  const data = plainTextData(timestamp, text);
  const ciphertext = aes128EcbEncrypt(secret32, data);
  const header = (PAYLOAD_TYPE_TXT_MSG << 2) | ROUTE_TYPE_FLOOD;
  const wire = Buffer.concat([
    Buffer.from([header, 0x00, recipientPublic[0], senderPublic[0]]),
    mac2(secret32, ciphertext),
    ciphertext,
  ]);
  const expectedAck = createHash("sha256")
    .update(data)
    .update(senderPublic)
    .digest()
    .subarray(0, 4);
  return { wire, expectedAck };
}

function verifyPinnedSourceContract() {
  const provenance = readFileSync(join(ROOT, "lib", "MeshCore", "UPSTREAM.md"), "utf8");
  const packetHeader = readFileSync(
    join(ROOT, "lib", "MeshCore", "src", "Packet.h"),
    "utf8",
  );
  const utils = readFileSync(
    join(ROOT, "lib", "MeshCore", "src", "Utils.cpp"),
    "utf8",
  );
  const dispatcherHeader = readFileSync(
    join(ROOT, "lib", "MeshCore", "src", "Dispatcher.h"),
    "utf8",
  );
  const transport = readFileSync(
    join(ROOT, "src", "kitsu_mesh_transport.cpp"),
    "utf8",
  );

  assert.match(provenance, /companion-v1\.17\.1/);
  assert.match(provenance, /d92964352441e53b93e8667b802e04f6e072b39e/);
  assert.match(packetHeader, /#define PAYLOAD_TYPE_TXT_MSG\s+0x02/);
  assert.match(packetHeader, /#define PAYLOAD_TYPE_GRP_TXT\s+0x05/);
  assert.match(utils, /AES128 aes;/);
  assert.match(utils, /aes\.setKey\(shared_secret, CIPHER_KEY_SIZE\)/);
  assert.match(utils, /sha\.resetHMAC\(shared_secret, PUB_KEY_SIZE\)/);
  assert.match(utils, /return CIPHER_MAC_SIZE \+ enc_len;/);
  assert.match(dispatcherHeader, /currentOutboundPacket\(\) const/);
  assert.match(transport, /pendingPacket_ && pendingPacket_ != inFlight/);
  assert.match(transport, /channelPacket_ && channelPacket_ != inFlight/);
}

function verifyPublicGroupVector() {
  assert.equal(
    PUBLIC_SECRET_32.toString("hex").toUpperCase(),
    "8B3387E9C5CDEA6AC9E5EDBAA115CD7200000000000000000000000000000000",
  );
  assert.equal(
    createHash("sha256").update(PUBLIC_PSK_16).digest()[0],
    0x11,
    "default Public channel hash changed",
  );

  const wire = buildGroupWire(GROUP_VECTOR.timestamp, GROUP_VECTOR.text);
  assert.equal(wire.toString("hex").toUpperCase(), GROUP_VECTOR.wireHex);
  assert.equal(wire[0], 0x15);
  assert.equal(wire[1], 0x00);
  assert.equal(wire[2], 0x11);

  const ciphertext = wire.subarray(5);
  assert.deepEqual(wire.subarray(3, 5), mac2(PUBLIC_SECRET_32, ciphertext));
  const clear = aes128EcbDecrypt(PUBLIC_SECRET_32, ciphertext);
  assert.equal(clear.readUInt32LE(0), GROUP_VECTOR.timestamp);
  assert.equal(clear[4] >> 2, TXT_TYPE_PLAIN);
  assert.equal(decodeZeroPaddedText(clear), GROUP_VECTOR.text);
}

function verifyDirectVector() {
  const secret = Buffer.from(Array.from({ length: 32 }, (_, index) => index));
  const senderPublic = Buffer.from(
    Array.from({ length: 32 }, (_, index) => 0xa1 + index),
  );
  const recipientPublic = Buffer.from(
    Array.from({ length: 32 }, (_, index) => 0xb2 + index),
  );
  const { wire, expectedAck } = buildDirectWire(
    DIRECT_VECTOR.timestamp,
    DIRECT_VECTOR.text,
    senderPublic,
    recipientPublic,
    secret,
  );

  assert.equal(wire.toString("hex").toUpperCase(), DIRECT_VECTOR.wireHex);
  assert.equal(expectedAck.toString("hex").toUpperCase(), DIRECT_VECTOR.ackHex);
  assert.equal(wire[0], 0x09);
  assert.equal(wire[1], 0x00);
  assert.equal(wire[2], recipientPublic[0]);
  assert.equal(wire[3], senderPublic[0]);

  const ciphertext = wire.subarray(6);
  assert.deepEqual(wire.subarray(4, 6), mac2(secret, ciphertext));
  const clear = aes128EcbDecrypt(secret, ciphertext);
  assert.equal(clear.readUInt32LE(0), DIRECT_VECTOR.timestamp);
  assert.equal(clear[4] >> 2, TXT_TYPE_PLAIN);
  assert.equal(decodeZeroPaddedText(clear), DIRECT_VECTOR.text);
}

verifyPinnedSourceContract();
verifyPublicGroupVector();
verifyDirectVector();

console.log("PASS MeshCore v1.17.1 Public-channel vector");
console.log("PASS MeshCore v1.17.1 direct-text/ACK vector");
console.log("PASS pinned-source text contract");
