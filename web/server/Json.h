#pragma once

#include "FloatBits.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

/*
 * Minimal append-only JSON writer.
 *
 * The server only ever serialises, never parses, so a full JSON library would
 * be dead weight. A single "does the next token need a leading comma" flag is
 * enough state: opening a container and writing a key both clear it, writing a
 * value or closing a container both set it.
 */
class Json {
public:
  const std::string& str() const {
    return out_;
  }

  std::string take() {
    return std::move(out_);
  }

  void beginObject() {
    sep();
    out_ += '{';
    needComma_ = false;
  }

  void endObject() {
    out_ += '}';
    needComma_ = true;
  }

  void beginArray() {
    sep();
    out_ += '[';
    needComma_ = false;
  }

  void endArray() {
    out_ += ']';
    needComma_ = true;
  }

  void key(std::string_view k) {
    sep();
    writeString(k);
    out_ += ':';
    needComma_ = false;
  }

  void value(std::string_view v) {
    sep();
    writeString(v);
  }

  void value(bool v) {
    sep();
    out_ += v ? "true" : "false";
  }

  void value(uint64_t v) {
    sep();
    out_ += std::to_string(v);
  }

  void value(uint32_t v) {
    value(static_cast<uint64_t>(v));
  }

  void value(int v) {
    sep();
    out_ += std::to_string(v);
  }

  /*
   * JSON has no NaN or Inf, so anything non-finite degrades to null rather
   * than producing a document the browser refuses to parse.
   *
   * The test goes through floatbits because this project builds with
   * -ffast-math, under which std::isfinite folds to a constant true and this
   * guard would never fire — the failure mode being a response body with a
   * bare `nan` token in it that no JSON parser accepts.
   */
  void value(double v) {
    sep();

    if (!floatbits::isFinite(v)) {
      out_ += "null";
      return;
    }

    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%.6g", v);
    out_.append(buf, static_cast<size_t>(n));
  }

  void null() {
    sep();
    out_ += "null";
  }

  template <typename T> void field(std::string_view k, T v) {
    key(k);
    value(v);
  }

  void nullField(std::string_view k) {
    key(k);
    null();
  }

private:
  void sep() {
    if (needComma_)
      out_ += ',';

    needComma_ = true;
  }

  void writeString(std::string_view v) {
    out_ += '"';

    for (const char c : v) {
      switch (c) {
      case '"':
        out_ += "\\\"";
        break;
      case '\\':
        out_ += "\\\\";
        break;
      case '\n':
        out_ += "\\n";
        break;
      case '\r':
        out_ += "\\r";
        break;
      case '\t':
        out_ += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out_ += buf;
        } else {
          out_ += c;
        }
      }
    }

    out_ += '"';
  }

  std::string out_;
  bool needComma_ = false;
};
