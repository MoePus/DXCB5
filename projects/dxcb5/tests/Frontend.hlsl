// Distributed under the University of Illinois Open Source License.

#if __SHADER_TARGET_MAJOR != EXPECTED_MAJOR || __SHADER_TARGET_MINOR != 0
#error Unexpected shader model macros
#endif

#if EXPECT_VERTEX
#if __SHADER_TARGET_STAGE != __SHADER_STAGE_VERTEX
#error Unexpected vertex shader stage macro
#endif
float4 shade(float4 position : POSITION, out float4 color : COLOR0)
    : SV_Position {
  color = position;
  return position;
}
#else
#if __SHADER_TARGET_STAGE != __SHADER_STAGE_PIXEL
#error Unexpected pixel shader stage macro
#endif
#if INCOMPATIBLE_COLOR
float4 shade(nointerpolation uint4 color : COLOR0) : SV_Target0 { return color; }
#elif TEST_GET_DIMENSIONS
Texture1D t1 : register(t0);
Texture1DArray t1a : register(t1);
Texture3D t3 : register(t2);
TextureCube tc : register(t3);
TextureCubeArray tca : register(t4);
Buffer<float4> tbuf : register(t5);
Texture2DMS<float4, 4> tms4 : register(t6);
Texture2DMS<float4> tms0 : register(t7);
Texture2DMSArray<float4, 4> tmsa : register(t8);
float4 shade(float4 color : COLOR0) : SV_Target0 {
  uint mip = (uint)color.x;
  uint w1, l1, w1a, a1a, l1a, w3, h3, d3, l3, wc, hc, lc, wca, hca, aca, lca;
  uint bw, m4w, m4h, m4s, m0w, m0h, m0s, maw, mah, maa, mas;
  t1.GetDimensions(mip, w1, l1);
  t1a.GetDimensions(mip, w1a, a1a, l1a);
  t3.GetDimensions(mip, w3, h3, d3, l3);
  tc.GetDimensions(mip, wc, hc, lc);
  tca.GetDimensions(mip, wca, hca, aca, lca);
  tbuf.GetDimensions(bw);
  tms4.GetDimensions(m4w, m4h, m4s);
  tms0.GetDimensions(m0w, m0h, m0s);
  tmsa.GetDimensions(maw, mah, maa, mas);
  return float4(w1 + w1a + w3 + wc + wca + bw + m4w + m0w + maw,
                a1a + h3 + hc + hca + m4h + m0h + mah,
                d3 + aca + maa,
                l1 + l1a + l3 + lc + lca + m4s + m0s + mas);
}
#elif TEST_INDEXABLE_TEMP
float4 shade(float4 color : COLOR0) : SV_Target0 {
  float arr[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint wi = (uint)color.x & 7u;
  uint ri = (uint)color.y & 7u;
  arr[wi] = color.z;
  return float4(arr[ri], arr[0], arr[7], 1.0);
}
#elif TEST_DOUBLE
float4 shade(float4 color : COLOR0) : SV_Target0 {
  double d = 2.0 + (double)color.x;
  return float4((float)(1.0 / d), 0, 0, 1);
}
#else
float4 shade(float4 color : COLOR0) : SV_Target0 { return color; }
#endif
#endif
