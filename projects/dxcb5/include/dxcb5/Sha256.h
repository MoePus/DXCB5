// Distributed under the University of Illinois Open Source License.
// Standalone FIPS 180-4 SHA-256 hasher for deterministic content-addressed
// cache keys (zero external/CryptoAPI dependencies).
#ifndef DXCB5_SHA256_H
#define DXCB5_SHA256_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace dxcb5 {

class Sha256 {
public:
  Sha256() { Reset(); }

  void Reset() {
    State_[0] = 0x6a09e667u;
    State_[1] = 0xbb67ae85u;
    State_[2] = 0x3c6ef372u;
    State_[3] = 0xa54ff53au;
    State_[4] = 0x510e527fu;
    State_[5] = 0x9b05688cu;
    State_[6] = 0x1f83d9abu;
    State_[7] = 0x5be0cd19u;
    BitCount_ = 0;
    BufLen_ = 0;
  }

  void Update(const void *Data, size_t Len) {
    if (!Data || Len == 0)
      return;
    const auto *P = static_cast<const uint8_t *>(Data);
    BitCount_ += static_cast<uint64_t>(Len) * 8u;
    if (BufLen_ > 0) {
      const size_t Need = 64u - BufLen_;
      const size_t Take = Len < Need ? Len : Need;
      std::memcpy(Buf_ + BufLen_, P, Take);
      BufLen_ += Take;
      P += Take;
      Len -= Take;
      if (BufLen_ == 64u) {
        Compress(Buf_);
        BufLen_ = 0;
      }
    }
    while (Len >= 64u) {
      Compress(P);
      P += 64u;
      Len -= 64u;
    }
    if (Len > 0) {
      std::memcpy(Buf_, P, Len);
      BufLen_ = Len;
    }
  }

  template <typename T>
  void UpdatePod(const T &V) {
    Update(&V, sizeof(T));
  }

  // Length-prefixed + delimiter-terminated string update to prevent
  // concatenation / boundary collisions between adjacent fields.
  void UpdateDelim(std::string_view S) {
    const uint64_t Len = static_cast<uint64_t>(S.size());
    UpdatePod(Len);
    if (!S.empty())
      Update(S.data(), S.size());
    const uint8_t Sep = 0x1Fu;
    Update(&Sep, 1);
  }

  void Finalize(uint8_t Out[32]) const {
    Sha256 Copy = *this;
    const uint8_t Pad80 = 0x80u;
    Copy.Buf_[Copy.BufLen_++] = Pad80;
    if (Copy.BufLen_ > 56u) {
      while (Copy.BufLen_ < 64u)
        Copy.Buf_[Copy.BufLen_++] = 0u;
      Copy.Compress(Copy.Buf_);
      Copy.BufLen_ = 0;
    }
    while (Copy.BufLen_ < 56u)
      Copy.Buf_[Copy.BufLen_++] = 0u;
    for (int I = 7; I >= 0; --I)
      Copy.Buf_[Copy.BufLen_++] = static_cast<uint8_t>((Copy.BitCount_ >> (I * 8)) & 0xFFu);
    Copy.Compress(Copy.Buf_);
    for (size_t I = 0; I < 8u; ++I) {
      Out[I * 4 + 0] = static_cast<uint8_t>((Copy.State_[I] >> 24) & 0xFFu);
      Out[I * 4 + 1] = static_cast<uint8_t>((Copy.State_[I] >> 16) & 0xFFu);
      Out[I * 4 + 2] = static_cast<uint8_t>((Copy.State_[I] >> 8) & 0xFFu);
      Out[I * 4 + 3] = static_cast<uint8_t>(Copy.State_[I] & 0xFFu);
    }
  }

  std::string HexBytes(size_t NumBytes) const {
    uint8_t Digest[32];
    Finalize(Digest);
    if (NumBytes > 32u)
      NumBytes = 32u;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string Out;
    Out.reserve(NumBytes * 2u);
    for (size_t I = 0; I < NumBytes; ++I) {
      Out.push_back(kHex[(Digest[I] >> 4) & 0xFu]);
      Out.push_back(kHex[Digest[I] & 0xFu]);
    }
    return Out;
  }

  // 128-bit truncated SHA-256 hex (32 chars): compact for Windows file paths.
  std::string Hex32() const { return HexBytes(16u); }

  // Full 256-bit SHA-256 hex (64 chars).
  std::string Hex64() const { return HexBytes(32u); }

private:
  static inline uint32_t Rotr(uint32_t X, uint32_t N) {
    return (X >> N) | (X << (32u - N));
  }

  void Compress(const uint8_t Block[64]) {
    static constexpr uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    uint32_t W[64];
    for (size_t I = 0; I < 16u; ++I) {
      W[I] = (static_cast<uint32_t>(Block[I * 4 + 0]) << 24) |
             (static_cast<uint32_t>(Block[I * 4 + 1]) << 16) |
             (static_cast<uint32_t>(Block[I * 4 + 2]) << 8) |
             static_cast<uint32_t>(Block[I * 4 + 3]);
    }
    for (size_t I = 16u; I < 64u; ++I) {
      const uint32_t S0 = Rotr(W[I - 15], 7) ^ Rotr(W[I - 15], 18) ^ (W[I - 15] >> 3);
      const uint32_t S1 = Rotr(W[I - 2], 17) ^ Rotr(W[I - 2], 19) ^ (W[I - 2] >> 10);
      W[I] = W[I - 16] + S0 + W[I - 7] + S1;
    }

    uint32_t A = State_[0], B = State_[1], C = State_[2], D = State_[3];
    uint32_t E = State_[4], F = State_[5], G = State_[6], H = State_[7];
    for (size_t I = 0; I < 64u; ++I) {
      const uint32_t S1 = Rotr(E, 6) ^ Rotr(E, 11) ^ Rotr(E, 25);
      const uint32_t Ch = (E & F) ^ ((~E) & G);
      const uint32_t Temp1 = H + S1 + Ch + K[I] + W[I];
      const uint32_t S0 = Rotr(A, 2) ^ Rotr(A, 13) ^ Rotr(A, 22);
      const uint32_t Maj = (A & B) ^ (A & C) ^ (B & C);
      const uint32_t Temp2 = S0 + Maj;
      H = G;
      G = F;
      F = E;
      E = D + Temp1;
      D = C;
      C = B;
      B = A;
      A = Temp1 + Temp2;
    }
    State_[0] += A;
    State_[1] += B;
    State_[2] += C;
    State_[3] += D;
    State_[4] += E;
    State_[5] += F;
    State_[6] += G;
    State_[7] += H;
  }

  uint32_t State_[8];
  uint64_t BitCount_ = 0;
  uint8_t Buf_[64];
  size_t BufLen_ = 0;
};

} // namespace dxcb5

#endif // DXCB5_SHA256_H
