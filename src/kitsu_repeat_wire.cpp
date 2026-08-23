#include "kitsu_repeat_wire.h"

namespace kitsu868 {
namespace mesh {

bool validFloodRouteBinding(const FloodRouteBinding& binding) {
  if (binding.pathHashSize < 1U || binding.pathHashSize > 3U) return false;
  if (!binding.transportScoped) {
    return binding.transportCodes[0] == 0U &&
        binding.transportCodes[1] == 0U;
  }
  // Correlation must retain even malformed/reserved received values so an
  // exact payload under the wrong code can be diagnosed as WireMismatch.
  // The transmit producer separately guarantees canonical non-reserved #EU
  // code0 and zero code1.
  return true;
}

bool sameFloodRouteBinding(const FloodRouteBinding& left,
                           const FloodRouteBinding& right) {
  return validFloodRouteBinding(left) && validFloodRouteBinding(right) &&
      left.transportScoped == right.transportScoped &&
      left.pathHashSize == right.pathHashSize &&
      left.transportCodes[0] == right.transportCodes[0] &&
      left.transportCodes[1] == right.transportCodes[1];
}

bool floodRouteBindingFromWire(const RepeatWireView& wire,
                               FloodRouteBinding& output) {
  output = FloodRouteBinding{};
  if (!wire.flood) return false;
  output.transportScoped = wire.hasTransportCodes;
  output.pathHashSize = wire.pathHashSize;
  if (wire.hasTransportCodes) {
    output.transportCodes[0] = wire.transportCodes[0];
    output.transportCodes[1] = wire.transportCodes[1];
  }
  return validFloodRouteBinding(output);
}

RepeatWireDecodeStatus decodeRepeatWire(const uint8_t* raw, size_t rawBytes,
                                        RepeatWireView& output) {
  output = RepeatWireView{};
  if (!raw || rawBytes < 2U) {
    return RepeatWireDecodeStatus::InvalidArgument;
  }
  const uint8_t header = raw[0];
  if (((header >> kRepeatWireVersionShift) & kRepeatWireVersionMask) !=
      kRepeatWirePayloadVersion1) {
    return RepeatWireDecodeStatus::UnsupportedVersion;
  }

  output.route = header & kRepeatWireRouteMask;
  output.payloadType = (header >> kRepeatWireTypeShift) & kRepeatWireTypeMask;
  output.flood = output.route == kRepeatWireRouteFlood ||
      output.route == kRepeatWireRouteTransportFlood;
  const bool transport = output.route == kRepeatWireRouteTransportFlood ||
      output.route == kRepeatWireRouteTransportDirect;
  size_t cursor = 1U;
  if (transport) {
    if (rawBytes < 6U) return RepeatWireDecodeStatus::TruncatedTransport;
    output.hasTransportCodes = true;
    output.transportCodes[0] = static_cast<uint16_t>(raw[cursor]) |
        static_cast<uint16_t>(raw[cursor + 1U]) << 8U;
    output.transportCodes[1] = static_cast<uint16_t>(raw[cursor + 2U]) |
        static_cast<uint16_t>(raw[cursor + 3U]) << 8U;
    cursor += 4U;
  }

  const uint8_t encodedPath = raw[cursor++];
  const uint8_t pathMode = encodedPath >> 6U;
  if (pathMode == 3U) return RepeatWireDecodeStatus::InvalidPath;
  output.pathHashSize = pathMode + 1U;
  output.pathCount = encodedPath & 63U;
  output.pathBytes =
      static_cast<size_t>(output.pathHashSize) * output.pathCount;
  if (output.pathBytes > kRepeatWireMaximumPathBytes ||
      cursor + output.pathBytes > rawBytes) {
    return RepeatWireDecodeStatus::InvalidPath;
  }
  output.path = raw + cursor;
  cursor += output.pathBytes;
  if (cursor >= rawBytes) return RepeatWireDecodeStatus::MissingPayload;
  output.payload = raw + cursor;
  output.payloadBytes = rawBytes - cursor;
  return RepeatWireDecodeStatus::Ok;
}

}  // namespace mesh
}  // namespace kitsu868
