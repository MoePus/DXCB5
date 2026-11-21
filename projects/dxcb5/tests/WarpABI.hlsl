// Distributed under the University of Illinois Open Source License.
struct Varyings {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  nointerpolation uint eye : SV_RenderTargetArrayIndex;
  float2 tint : TEXCOORD1;
};

Varyings VS(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
  Varyings output;
  output.position = float4(vertex == 2 ? 3 : -1, vertex == 1 ? 3 : -1, 0, 1);
  output.uv = float2(0.125, 0.25);
  output.tint = float2(0.0625, 0.125);
  output.eye = instance;
  return output;
}

Texture2D<float4> TestTexture : register(t3);
SamplerState TestSampler : register(s2);
struct MaterialData {
  row_major float3x3 transform;
  float4 bias;
};
cbuffer TestBuffer : register(b4) {
  float2 TintScale : packoffset(c0.z) = float2(0.25, 0.5);
  float4 Colors[3] : packoffset(c1);
  MaterialData Materials[2] : packoffset(c4);
};

float4 PS(Varyings input) : SV_Target {
#if DXCB5_TEST_DISCARD
  if ((uint(input.position.x) & 1) == 0)
    discard;
#endif
  float4 sample = TestTexture.SampleLevel(TestSampler, input.uv, 0, int2(1, 0));
  static const float Weights[8] = {0.03125, 0.0625, 0.09375, 0.125,
                                  0.15625, 0.1875, 0.21875, 0.25};
  uint weightIndex = (input.eye + uint(input.uv.x * input.uv.y)) & 7;
  float4 color = Colors[input.eye + 1];
  float4 tint = float4(input.tint * TintScale, 0, 0);
  tint.w = Materials[input.eye].transform[1][1] + Materials[input.eye].bias.x;
  tint.z = TestTexture.Load(int3(0, 0, 1)).z;
  tint.y += Weights[weightIndex];
  [branch] if (input.eye) return sample + color + tint;
  return sample * color + tint;
}

#if DXCB5_TEST_OVERFLOW_RANGE
Texture2D<float4> TestTextures[3] : register(t127);
#else
Texture2D<float4> TestTextures[3] : register(t11);
#endif
SamplerState TestSamplers[2][2] : register(s5);
Texture2DArray<float4> TestDepthArray : register(t17);
Texture2DArray<float4> TestGatherArray : register(t18);

float4 PSGather(Varyings input) : SV_Target {
  float4 mono = TestTexture.GatherRed(TestSampler, float2(0.5, 0.5));
  float4 stereo = TestGatherArray.GatherGreen(
      TestSampler, float3(0.25, 0.5, input.eye), int2(1, 0));
  return mono + stereo;
}

cbuffer QuadLoopControl : register(b0) {
  uint QuadLoopIterations;
};

float4 PSQuadLoop(Varyings input) : SV_Target {
  float lane = input.position.x;
  float sum = 0;
  float sampled = 0;
  [loop] for (uint i = 0; i < input.eye * QuadLoopIterations; ++i) {
    sum += ddx_fine(lane);
    // At the readback pixels the final iteration selects mip 1.
    sampled += TestTexture.Sample(TestSampler, float2(lane * 0.25, 0)).z;
    lane *= 2;
  }
  return float4(sum, sampled, 0, 1);
}

float4 PSLocalArrays(Varyings input) : SV_Target {
  uint x = uint(input.position.x);
  float2 small = float2(input.position.y, input.position.y + 4);
  float4 wide = float4(input.position.x, input.position.x + 1,
                       input.position.x + 2, input.position.x + 3);
  if ((x & 1) != 0) {
    small.y += 8;
    wide.z += 16;
  }
  uint index = x & 3;
  float sum = 0;
  uint iterations = (x & 1) + 2;
  [loop] for (uint step = 0; step < iterations; ++step) {
    sum += wide[(index + step) & 3];
    wide.y += 0.25;
  }
  return float4(small[index & 1], wide[index], sum, 1);
}

float4 PSResources(Varyings input) : SV_Target {
#if DXCB5_TEST_DYNAMIC_SRV
  uint textureIndex = input.eye;
#else
  uint textureIndex = 2;
#endif
#if DXCB5_TEST_DYNAMIC_SAMPLER
  uint samplerIndex = input.eye;
#else
  uint samplerIndex = 1;
#endif
  uint width, height, mipWidth, mipHeight, mipCount;
  TestTextures[0].GetDimensions(width, height);
  TestTextures[textureIndex].GetDimensions(input.eye, mipWidth, mipHeight, mipCount);
  uint arrayWidth, arrayHeight, slices, arrayMips;
  TestDepthArray.GetDimensions(input.eye, arrayWidth, arrayHeight, slices, arrayMips);
  return float4(
      TestTextures[0].SampleLevel(TestSamplers[0][0], input.uv, 0).x + width + arrayWidth,
      TestTextures[textureIndex].SampleLevel(TestSamplers[samplerIndex][1], input.uv, 0).y + height + arrayHeight,
      TestTextures[0].SampleLevel(TestSamplers[0][0], float2(-1, -1), 0).z + mipWidth + slices,
      mipHeight + mipCount * 8 + arrayMips +
      TestTextures[textureIndex].SampleLevel(TestSamplers[samplerIndex][1], float2(-1, -1), 0).w);
}

float ReadByValueArray(float a[6], uint idx) {
  return a[idx];
}

float4 PSCorrectness(Varyings input) : SV_Target {
  uint col = uint(input.position.x) & 3;
  [branch] if (col == 0) {
    if (input.eye != 0) {
      // SM5 UBFE, BFI, integer sub (-r), and IEEE empty-clamp collapse.
      float nanV = log2(input.position.x - 8.5);
      float eclamp = min(max(nanV, 0.0001), 0.0);
      uint ux = uint(input.position.x * 137.0) + 0x12345u;
      uint uy = uint(input.position.y * 53.0) + 0x9876u;
      uint ub = (ux >> 7) & 31u;
      uint bf = ((ux << 2) & 4u) | ((uy & ~992u) | ((ux << 5) & 992u));
      int subV = int(ux) - int(uy);
      return float4(eclamp, float(ub), float(bf & 0xffffu), float(subV & 0xffff));
    }
    // Vector immediate negation under -Od and -O3.
    return input.position.xyxy - float4(1.0, 2.0, 3.0, 4.0);
  }
  [branch] if (col == 1) {
    // uitofp i1 -> float and dynamic array write whose index is also output.
    float f0 = float(input.eye == 0);
    float f1 = float(input.eye != 0);
    float vals[5] = {10.0, 20.0, 30.0, 40.0, 50.0};
    uint idx = input.eye + 1;
    vals[idx] = 99.0;
    return float4(f0, f1, float(idx), vals[input.eye + 1]);
  }
  [branch] if (col == 2) {
    // IEEE/SM5 float edge cases + SM5 bitfield/int-sub verification.
    float neg = input.position.x - 8.5;
    float p = exp2(2.0 * log2(neg));
    float3 z3 = float3(input.position.x - 6.5, 0.0, 0.0);
    float dn = dot(normalize(z3), normalize(z3));
    float nanV = log2(neg);
    float cs = (2.0 < nanV) ? 2.0 : nanV;
    float bigA = (input.position.x - 5.5) * 33554432.0;
    float t = (input.eye == 0) ? 1.0 : 0.0;
    float lr = (1.0 - bigA) * t + bigA;
    if (input.eye != 0) {
      float bmul = ((input.eye == 99u) ? 1.0 : 0.0) * nanV;
      float mclamp = max(min(nanV, 1.0), 0.0);
      return float4(bmul, mclamp, cs, lr);
    }
    return float4(p, dn, cs, lr);
  }
  // Duplicate writes + DomSpill across branches + pass-by-value array copy + [2 x float] dyn store.
  float arr[6] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  arr[0] = 42.0;
  arr[0] = 42.0;
  [branch] if (input.eye != 0) {
    arr[1] = 20.0;
    arr[2] = 30.0;
  } else {
    arr[1] = 10.0;
    arr[2] = 15.0;
  }
  float picked = ReadByValueArray(arr, input.eye);
  float pair[2] = {7.0, 8.0};
  pair[input.eye] = 55.0;
  return float4(picked, arr[2], pair[0], pair[1]);
}

Buffer<float4> DimBuffer : register(t19);
Texture2DMS<float4, 4> DimMS4 : register(t20);
Texture2DMS<float4> DimMS0 : register(t21);
Texture2DMSArray<float4, 4> DimMSA : register(t22);

float4 PSDimensions(Varyings input) : SV_Target {
  uint bw, m4w, m4h, m4s, m0w, m0h, m0s, maw, mah, maa, mas;
  DimBuffer.GetDimensions(bw);
  DimMS4.GetDimensions(m4w, m4h, m4s);
  DimMS0.GetDimensions(m0w, m0h, m0s);
  DimMSA.GetDimensions(maw, mah, maa, mas);
  return float4(bw + m4w + m0w + maw, m4h + m0h + mah, maa, m4s + m0s + mas);
}

struct CompatItem {
  float4 a;
  uint2 b;
};

Buffer<float4> CompatTypedBuf : register(t23);
StructuredBuffer<CompatItem> CompatStructBuf : register(t24);
ByteAddressBuffer CompatRawBuf : register(t25);

struct CompatOut {
  float4 color : SV_Target0;
  float depth : SV_Depth;
};

CompatOut PSCompat(Varyings input) {
  uint col = uint(input.position.x) & 3u;
  CompatOut o;
  // Even columns emit depth 0.25 (passes depth<0.5), odd columns emit depth 0.75 (rejected).
  o.depth = (col & 1u) ? 0.75 : 0.25;
  if (col == 0) {
    // Signed division & remainder across signs and INT32_MIN.
    int a = (input.eye == 0) ? -7 : int(0x80000000u);
    int b = (input.eye == 0) ? 3 : -2;
    int q = a / b;
    int r = a % 3;
    int a2 = (input.eye == 0) ? 7 : -7;
    int b2 = -3;
    o.color = float4(float(q), float(r), float(a2 / b2), float(a2 % b2));
    return o;
  }
  // Buffer<float4>, StructuredBuffer<CompatItem>, ByteAddressBuffer loads + GetDimensions.
  uint idx = input.eye;
  float4 tbv = CompatTypedBuf[idx];
  CompatItem sbv = CompatStructBuf[idx];
  uint2 bbv = CompatRawBuf.Load2(idx * 8u);
  uint sbNum, sbStride, bbBytes;
  CompatStructBuf.GetDimensions(sbNum, sbStride);
  CompatRawBuf.GetDimensions(bbBytes);
  o.color = float4(tbv.x + sbv.a.y, float(sbv.b.y) + float(bbv.x),
                   float(bbv.y), float(sbNum + sbStride + bbBytes));
  return o;
}

Texture2D<float> CompatCmpTex : register(t26);
Texture2DMS<float4, 4> CompatMSTex : register(t27);
SamplerComparisonState CompatCmpSampler : register(s9);

float4 PSCompat2(Varyings input, uint sampleIdx : SV_SampleIndex) : SV_Target {
  float lodC = TestTexture.CalculateLevelOfDetail(TestSampler, input.uv);
  float lodU = TestTexture.CalculateLevelOfDetailUnclamped(TestSampler, input.uv);
  float2 ec = EvaluateAttributeCentroid(input.tint);
  float2 es = EvaluateAttributeAtSample(input.tint, sampleIdx);
  float2 en = EvaluateAttributeSnapped(input.tint, int2(2, -3));
  uint col = uint(input.position.x) & 3u;
  if (col == 0) {
    // uaddc (AddUint64) with carry=1 and carry=0.
    uint2 r1 = AddUint64(uint2(0xfffffffeu, 10u), uint2(3u + input.eye, 20u));
    uint2 r2 = AddUint64(uint2(5u + input.eye, 10u), uint2(3u, 20u));
    return float4(float(r1.x), float(r1.y), float(r2.x), float(r2.y));
  }
  if (col == 1) {
    // gather4_po with dynamic offset (1+eye, 0).
    return TestGatherArray.GatherGreen(
        TestSampler, float3(0.25, 0.5, input.eye), int2(int(input.eye) + 1, 0));
  }
  if (col == 2) {
    // gather4_c + gather4_po_c + CalculateLevelOfDetail (clamped & unclamped).
    float4 gc = CompatCmpTex.GatherCmpRed(
        CompatCmpSampler, float2(0.25, 0.5), 0.5, int2(1, 0));
    float4 gpoc = CompatCmpTex.GatherCmpRed(
        CompatCmpSampler, float2(0.25, 0.5), 0.5, int2(int(input.eye) + 1, 0));
    return float4(dot(gc, 1.0), dot(gpoc, 1.0), lodC, max(lodU, -8.0));
  }
  // ldms + eval_centroid + eval_sample_index + eval_snapped + SV_SampleIndex.
  float4 ms = CompatMSTex.Load(int2(0, 0), int(input.eye));
  return float4(ms.x + ec.x, ms.y + es.y, ms.z + en.x, ms.w + float(sampleIdx));
}

struct CompatUavItem {
  float2 a;
  uint b;
};

RWBuffer<float4> CompatUavTyped : register(u1);
RWTexture2D<uint> CompatUavTex : register(u2);
RWStructuredBuffer<CompatUavItem> CompatUavStruct : register(u3);
RWByteAddressBuffer CompatUavRaw : register(u4);
RWStructuredBuffer<uint> CompatUavCnt : register(u5);

float4 PSCompat3(Varyings input) : SV_Target {
  uint col = uint(input.position.x) & 3u;
  uint row = uint(input.position.y);
  if (col == 0) {
    // min16float, min16int (signed shift & cmp), min16uint (unsigned shift).
    min16float f = min16float(input.uv.x) * min16float(4.0) + min16float(1.5);
    min16int si = min16int(input.eye) - min16int(5);
    int s1 = int(si >> 1);
    int s2 = (si < min16int(-4)) ? 10 : 20;
    min16uint ui = min16uint(0xfff0u) + min16uint(input.eye);
    uint u1 = uint(ui >> 4);
    return float4(float(f), float(s1), float(s2), float(u1));
  }
  if (col == 1) {
    float4 tv = CompatUavTyped[input.eye];
    uint prev = 0;
    if (row == 4 && uint(input.position.x) == 5) {
      CompatUavTyped[2 + input.eye] = float4(10.0 + float(input.eye), 20.0, 30.0, 40.0);
      InterlockedAdd(CompatUavTex[uint2(input.eye, 0)], 5u, prev);
    }
    return float4(tv.x, tv.y, tv.z, float(prev));
  }
  if (col == 2) {
    CompatUavItem it = CompatUavStruct[input.eye];
    uint2 rv = CompatUavRaw.Load2(input.eye * 8u);
    uint sn, ss, rb;
    CompatUavStruct.GetDimensions(sn, ss);
    CompatUavRaw.GetDimensions(rb);
    uint oldB = 0;
    if (row == 4 && uint(input.position.x) == 6) {
      InterlockedCompareExchange(CompatUavStruct[input.eye].b, 7u, 99u, oldB);
      CompatUavRaw.Store2(16u + input.eye * 8u, uint2(rv.x + 1u, rv.y + 2u));
    }
    return float4(it.a.x + it.a.y, float(oldB), float(rv.x + rv.y),
                  float(sn + ss + rb));
  }
  uint cidx = 0;
  if (row == 4 && uint(input.position.x) == 7 && input.eye == 0) {
    cidx = CompatUavCnt.IncrementCounter();
    CompatUavCnt[cidx] = 777u;
  }
  return float4(float(cidx), 1.0, 2.0, 3.0);
}

static float gMutableArr[4] = {10.0, 20.0, 30.0, 40.0};

void PSCompat4(Varyings input, inout uint cov : SV_Coverage,
               out float4 color : SV_Target0) {
  uint col = uint(input.position.x) & 3u;
  // Mutate static global array at dynamic index and read back + bool local array.
  gMutableArr[input.eye] = gMutableArr[input.eye + 2] + 5.0;
  bool flags[4] = {true, false, true, false};
  bool fl = flags[col];
  // Zero out coverage mask on odd columns so those pixels remain ClearColor.
  cov = (col & 1u) ? 0u : cov;
  color = float4(gMutableArr[0], gMutableArr[1], float(cov), fl ? 1.0 : 0.0);
}
