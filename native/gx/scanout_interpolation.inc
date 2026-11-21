// SPDX-License-Identifier: MIT
// Included in Aurora's gfx namespace after the deferred presentation helpers.
// These inputs are completed scanout images. The guest EFB and its copies are
// never used as scratch targets for an interpolated image.
namespace {
constexpr uint32_t MotionTile = 8;
constexpr uint32_t MaxGenerated = 11; // 30 real frames/sec to 360 presents/sec
constexpr char ScanoutInterpolationShader[] = R"(
@group(0) @binding(0) var prior: texture_2d<f32>;
@group(0) @binding(1) var next: texture_2d<f32>;
@group(0) @binding(2) var flowOut: texture_storage_2d<rgba16float, write>;
@group(0) @binding(3) var flow: texture_2d<f32>;
@group(0) @binding(4) var output: texture_storage_2d_array<rgba8unorm, write>;
@group(0) @binding(5) var cutOut: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(6) var cut: texture_2d<f32>;
@group(0) @binding(7) var sampleFilter: sampler;
@group(0) @binding(8) var<uniform> phases: vec4u;

fn bounded(p: vec2i, dims: vec2i) -> vec2i {
    return clamp(p, vec2i(0), dims - vec2i(1));
}

fn luma(rgb: vec3f) -> f32 {
    return dot(rgb, vec3f(0.2126, 0.7152, 0.0722));
}

fn error_at(p: vec2i, h: vec2i, dims: vec2i) -> f32 {
    var error = 0.0;
    for (var y = -3; y <= 3; y += 3) {
        for (var x = -3; x <= 3; x += 3) {
            let q = p + vec2i(x, y);
            let a = textureLoad(prior, bounded(q - h, dims), 0).rgb;
            let b = textureLoad(next, bounded(q + h, dims), 0).rgb;
            let difference = abs(a - b);
            error += luma(difference) + 0.25 * max(max(difference.r, difference.g), difference.b);
        }
    }
    return error / 9.0;
}

@compute @workgroup_size(8, 8)
fn estimate(@builtin(global_invocation_id) id: vec3u) {
    if (any(id.xy >= textureDimensions(flowOut))) { return; }
    let dims = vec2i(textureDimensions(next));
    let p = bounded(vec2i(id.xy * 8u + 4u), dims);
    var best = vec2i(0);
    let stationary = error_at(p, best, dims);
    var bestScore = stationary;
    // Smooth a transport that moves both sources toward the output pixel.
    // Bias ties toward no motion to keep still elements such as the HUD sharp.
    if (stationary > 0.015) {
        for (var y = -8; y <= 8; y += 4) {
            for (var x = -8; x <= 8; x += 4) {
                let h = vec2i(x, y);
                let score = error_at(p, h, dims) + 0.0025 * f32(abs(x) + abs(y));
                if (score < bestScore) { best = h; bestScore = score; }
            }
        }
        for (var stride = 2; stride >= 1; stride /= 2) {
            let center = best;
            for (var y = -1; y <= 1; y += 1) {
                for (var x = -1; x <= 1; x += 1) {
                    let h = clamp(center + vec2i(x, y) * stride, vec2i(-10), vec2i(10));
                    let score = error_at(p, h, dims) + 0.0025 * f32(abs(h.x) + abs(h.y));
                    if (score < bestScore) { best = h; bestScore = score; }
                }
            }
        }
    }
    textureStore(flowOut, vec2i(id.xy), vec4f(vec2f(best), stationary, bestScore));
}

@compute @workgroup_size(1)
fn scene() {
    let dims = textureDimensions(flow);
    var changed = 0u;
    var unmatched = 0u;
    for (var y = 0u; y < 8u; y += 1u) {
        for (var x = 0u; x < 8u; x += 1u) {
            let p = vec2i((vec2u(x, y) * 2u + 1u) * dims / 16u);
            let m = textureLoad(flow, p, 0);
            changed += select(0u, 1u, m.z > 0.14);
            unmatched += select(0u, 1u, m.w > 0.10);
        }
    }
    let reject = changed >= 36u && unmatched >= 25u;
    textureStore(cutOut, vec2i(0), vec4f(select(0.0, 1.0, reject), 0.0, 0.0, 1.0));
}

@compute @workgroup_size(8, 8)
fn synthesize(@builtin(global_invocation_id) id: vec3u) {
    let dims = vec2i(textureDimensions(output));
    let p = vec2i(id.xy);
    if (any(p >= dims)) { return; }
    let before = textureLoad(prior, p, 0);
    let after = textureLoad(next, p, 0);
    let time = (f32(id.z) + 1.0) / (f32(phases.x) + 1.0);
    let nearest = select(before, after, time > 0.5);
    if (textureLoad(cut, vec2i(0), 0).r != 0.0) {
        textureStore(output, p, i32(id.z), nearest);
        return;
    }
    let center = vec2f(p) + 0.5;
    let m = textureSampleLevel(flow, sampleFilter, center / (8.0 * vec2f(textureDimensions(flow))), 0.0);
    let uvScale = 1.0 / vec2f(dims);
    let movedBefore = textureSampleLevel(prior, sampleFilter, (center - 2.0 * time * m.xy) * uvScale, 0.0);
    let movedAfter = textureSampleLevel(next, sampleFilter, (center + 2.0 * (1.0 - time) * m.xy) * uvScale, 0.0);
    let stationary = max(max(abs(before.r - after.r), abs(before.g - after.g)), abs(before.b - after.b));
    let mismatch = luma(abs(movedBefore.rgb - movedAfter.rgb));
    // Fade unreliable motion per pixel. A hard cutoff at the edge of a
    // motion tile makes the synthesized image look blocky on camera turns.
    let confidence = smoothstep(0.018, 0.055, stationary) *
        (1.0 - smoothstep(0.05, 0.13, m.w)) *
        (1.0 - smoothstep(0.055, 0.20, mismatch));
    textureStore(output, p, i32(id.z), mix(nearest, mix(movedBefore, movedAfter, time), confidence));
}
)";

struct ScanoutInterpolation {
  wgpu::ComputePipeline estimate, scene, synthesize;
  wgpu::Sampler linear;
  wgpu::TextureView flow, cut, midpoint;
  wgpu::Texture midpointTexture;
  wgpu::Buffer phaseUniform;
  uint32_t width = 0, height = 0, layers = 0;
  std::array<std::array<webgpu::TextureWithSampler, MaxGenerated+1>, 2> held;
  unsigned heldSet = 0;
  bool hasPrevious = false;
  double midpointBudget = 0.0;
  PresentClock::time_point lastFrame{};

  void prepare(const webgpu::TextureWithSampler& source, uint32_t count) {
    if (!estimate) {
      wgpu::ShaderSourceWGSL wgsl;
      wgsl.code = ScanoutInterpolationShader;
      const wgpu::ShaderModuleDescriptor shader{.nextInChain = &wgsl};
      const auto module = g_device.CreateShaderModule(&shader);
      const auto pipeline = [&module](const char* name) {
        const wgpu::ComputePipelineDescriptor desc{.label = name, .compute = {.module = module, .entryPoint = name}};
        return g_device.CreateComputePipeline(&desc);
      };
      estimate = pipeline("estimate");
      scene = pipeline("scene");
      synthesize = pipeline("synthesize");
      const wgpu::SamplerDescriptor sampler{.magFilter = wgpu::FilterMode::Linear,
          .minFilter = wgpu::FilterMode::Linear};
      linear = g_device.CreateSampler(&sampler);
      const wgpu::BufferDescriptor uniform{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
          .size = sizeof(uint32_t) * 4};
      phaseUniform = g_device.CreateBuffer(&uniform);
    }
    const auto texture = [](uint32_t w, uint32_t h, uint32_t depth, wgpu::TextureFormat format) {
      const wgpu::TextureDescriptor desc{
          .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::CopySrc,
          .size = {w, h, depth}, .format = format};
      return g_device.CreateTexture(&desc);
    };
    if (width != source.size.width || height != source.size.height) {
      width = source.size.width;
      height = source.size.height;
      flow = texture((width + MotionTile - 1) / MotionTile, (height + MotionTile - 1) / MotionTile,
                     1, wgpu::TextureFormat::RGBA16Float).CreateView();
      cut = texture(1, 1, 1, wgpu::TextureFormat::RGBA8Unorm).CreateView();
      layers = 0;
    }
    if (layers < count) {
      layers = count;
      midpointTexture = texture(width, height, layers, wgpu::TextureFormat::RGBA8Unorm);
      const wgpu::TextureViewDescriptor arrayView{.dimension = wgpu::TextureViewDimension::e2DArray,
          .arrayLayerCount = layers};
      midpoint = midpointTexture.CreateView(&arrayView);
    }
  }

  void encode(wgpu::CommandEncoder& encoder, const webgpu::TextureWithSampler& prior,
              const webgpu::TextureWithSampler& next,
              const std::array<webgpu::TextureWithSampler, MaxGenerated+1>& targets, uint32_t count) {
    const auto dispatch = [&encoder](const wgpu::ComputePipeline& pipeline,
                                    const auto& entries, uint32_t x, uint32_t y, uint32_t z,
                                    const char* name) {
      const wgpu::BindGroupDescriptor desc{.layout = pipeline.GetBindGroupLayout(0),
          .entryCount = entries.size(), .entries = entries.data()};
      const wgpu::ComputePassDescriptor passDesc{.label = name,
          .timestampWrites = webgpu::gpu_prof::pass_writes(name)};
      auto pass = encoder.BeginComputePass(&passDesc);
      pass.SetPipeline(pipeline);
      pass.SetBindGroup(0, g_device.CreateBindGroup(&desc));
      pass.DispatchWorkgroups(x, y, z);
      pass.End();
    };
    const std::array<uint32_t, 4> uniform{count,0,0,0};
    g_queue.WriteBuffer(phaseUniform, 0, uniform.data(), sizeof(uniform));
    const uint32_t groupsX = (width + 7) / 8, groupsY = (height + 7) / 8;
    dispatch(estimate, std::array{wgpu::BindGroupEntry{.binding = 0, .textureView = prior.view},
        wgpu::BindGroupEntry{.binding = 1, .textureView = next.view},
        wgpu::BindGroupEntry{.binding = 2, .textureView = flow}}, (groupsX + 7) / 8, (groupsY + 7) / 8,
        1, "Scanout motion");
    dispatch(scene, std::array{wgpu::BindGroupEntry{.binding = 3, .textureView = flow},
        wgpu::BindGroupEntry{.binding = 5, .textureView = cut}}, 1, 1, 1, "Scanout scene");
    dispatch(synthesize, std::array{wgpu::BindGroupEntry{.binding = 0, .textureView = prior.view},
        wgpu::BindGroupEntry{.binding = 1, .textureView = next.view},
        wgpu::BindGroupEntry{.binding = 3, .textureView = flow},
        wgpu::BindGroupEntry{.binding = 4, .textureView = midpoint},
        wgpu::BindGroupEntry{.binding = 6, .textureView = cut},
        wgpu::BindGroupEntry{.binding = 7, .sampler = linear},
        wgpu::BindGroupEntry{.binding = 8, .buffer = phaseUniform, .size = sizeof(uniform)}},
        groupsX, groupsY, count, "Scanout synthesis");
    const wgpu::Extent3D extent{width, height, 1};
    for (uint32_t i = 0; i < count; ++i) {
      const wgpu::TexelCopyTextureInfo src{.texture = midpointTexture, .origin = {0,0,i}};
      const wgpu::TexelCopyTextureInfo dst{.texture = targets[i].texture};
      encoder.CopyTextureToTexture(&src, &dst, &extent);
    }
  }
} g_scanoutInterpolation;
} // namespace

static void release_machine_interpolation() { g_scanoutInterpolation = {}; }

static void defer_scanout_phase(PresentClock::time_point due, PresentClock::duration spacing,
                                const webgpu::TextureWithSampler* source,
                                const EndFrameCallback& callback, uint64_t frameId, int step) {
  g_deferredPresents.push_back(
      DeferredPresent{.due = due, .source = source, .callback = callback,
                      .gameFrame = frameId, .step = step, .discardAfter = due + spacing});
}

static bool present_interpolated_scanout(wgpu::CommandEncoder& encoder,
                                        const EndFrameCallback& callback, uint64_t frameId, uint32_t targetFps) {
  const auto& source = webgpu::present_source();
  if (!source.texture || source.format != wgpu::TextureFormat::RGBA8Unorm)
    return false;
  auto& interpolation = g_scanoutInterpolation;
  const auto now = PresentClock::now();
  const auto interval = now - interpolation.lastFrame;
  interpolation.lastFrame = now;
  const auto spacing = std::chrono::nanoseconds(1'000'000'000 / targetFps);
  const bool continuous = interpolation.hasPrevious && interval < std::chrono::milliseconds(70);
  // Real scanouts take the first opportunity. Extra scanouts during a
  // transition reduce the time available for generated presentations.
  interpolation.midpointBudget = continuous
      ? std::clamp(interpolation.midpointBudget +
          std::chrono::duration<double>(interval).count() * targetFps - 1.0, -2.0, MaxGenerated + 1.0)
      : 0.0;
  const auto& previous = interpolation.held[interpolation.heldSet][MaxGenerated];
  const bool generate = continuous && interpolation.midpointBudget >= 1.0 &&
      previous.size.width == source.size.width && previous.size.height == source.size.height &&
      g_gameFramePeriodNs >= 11'000'000 && g_gameFramePeriodNs <= 39'500'000;
  const uint32_t count = generate
      ? std::min(MaxGenerated, static_cast<uint32_t>(interpolation.midpointBudget)) : 0;
  interpolation.hasPrevious = true;
  interpolation.heldSet ^= 1;
  auto& held = interpolation.held[interpolation.heldSet];
  flush_presents_from(&held.front(), &held.back());
  ensure_held_texture(held.back(), source);
  const wgpu::TexelCopyTextureInfo src{.texture = source.texture}, dst{.texture = held.back().texture};
  const wgpu::Extent3D extent{source.size.width, source.size.height, 1};
  encoder.CopyTextureToTexture(&src, &dst, &extent);
  if (!count) {
    // Pending presents temporarily replace the scanout override. Restore this
    // packet's source before its ordinary real-frame callback runs.
    run_deferred_present();
    webgpu::set_present_source_override(&source);
    g_lastDue = {};
    return false;
  }

  interpolation.midpointBudget -= count;
  interpolation.prepare(source, count);
  for (uint32_t i = 0; i < count; ++i)
    ensure_held_texture(held[i], source);
  interpolation.encode(encoder, previous, source, held, count);
  webgpu::gpu_prof::frame_end(encoder);
  const auto commands = encoder.Finish();
  g_queue.Submit(1, &commands);
  webgpu::gpu_prof::after_submit();
  after_submit();
  if (g_presentLog)
    std::fprintf(stderr, "[scanout-interpolation] frame=%llu target=%u phases=%u encode_us=%lld\n",
                 static_cast<unsigned long long>(frameId), targetFps, count,
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                     PresentClock::now() - now).count()));
  if (frame_interp::dump_frame(frameId)) {
    dump_texture(previous, "prior", frameId);
    dump_texture(held[0], "between", frameId);
    dump_texture(held.back(), "real", frameId);
  }
  const auto start = PresentClock::now();
  const auto due = g_lastDue.time_since_epoch().count() != 0
      ? std::clamp(g_lastDue + spacing, start, start + spacing) : start;
  for (uint32_t i = 0; i < count; ++i)
    defer_scanout_phase(due + i*spacing, spacing, &held[i], callback, frameId, static_cast<int>(i));
  g_lastDue = due + count*spacing;
  defer_present(g_lastDue, &held.back(), callback, frameId, -1);
  deferred_present_tick();
  return true;
}
