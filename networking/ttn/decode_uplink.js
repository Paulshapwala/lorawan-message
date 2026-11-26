/**
 * The Things Stack uplink formatter for compact destination tags and older text packets.
 */

/**
 * Map each byte at or after the given index to a JavaScript character.
 * @param {number[]} bytes - The uplink payload bytes.
 * @param {number} start - The index of the first byte to include.
 * @returns {string} The resulting text.
 */
function bytesToText(bytes, start) {
  var text = "";
  for (var i = start; i < bytes.length; i++) {
    text += String.fromCharCode(bytes[i]);
  }
  return text;
}

/**
 * Decode FPort 2 destination tags, older to=<hex>|text packets, and plain text.
 * @param {{fPort: number, bytes: number[]}} input - The Things Stack uplink.
 * @returns {object} Decoded data or validation errors in The Things Stack's formatter format.
 */
function decodeUplink(input) {
  if (input.fPort === 2) {
    if (input.bytes.length < 2 || input.bytes[0] === 0) {
      return { errors: ["Expected a destination byte from 1 to 255 followed by a message."] };
    }

    var tag = input.bytes[0];
    var deviceId = "custom-" + tag;
    var deviceIdHex = "";
    for (var i = 0; i < deviceId.length; i++) {
      deviceIdHex += deviceId.charCodeAt(i).toString(16);
    }

    return {
      data: {
        target_device_id: deviceId,
        target_device_id_hex: deviceIdHex,
        target_tag: ("0" + tag.toString(16)).slice(-2),
        message: bytesToText(input.bytes, 1)
      }
    };
  }

  var text = bytesToText(input.bytes, 0);

  if (text.indexOf("to=") !== 0) {
    return { data: { message: text } };
  }

  var separator = text.indexOf("|");
  var targetHex = text.slice(3, separator);
  if (separator < 0 || !/^(?:[0-9a-fA-F]{2}){3,36}$/.test(targetHex)) {
    return { errors: ["Invalid hex target device tag."] };
  }

  var targetId = "";
  for (var offset = 0; offset < targetHex.length; offset += 2) {
    targetId += String.fromCharCode(parseInt(targetHex.slice(offset, offset + 2), 16));
  }
  if (!/^[a-z0-9](?:-?[a-z0-9]){2,}$/.test(targetId)) {
    return { errors: ["Invalid TTN target device ID."] };
  }

  return {
    data: {
      target_device_id: targetId,
      target_device_id_hex: targetHex.toLowerCase(),
      message: text.slice(separator + 1)
    }
  };
}
