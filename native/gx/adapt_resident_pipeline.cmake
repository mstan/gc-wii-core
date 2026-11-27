# SPDX-License-Identifier: MIT
# Native material variants are prepared once, without a temporary generic
# shader fallback. Reuse the pipeline cache's existing blocking owner.
string(PREPEND _gcnllr_pipeline "#include <map>\n")
string(APPEND _gcnllr_pipeline [=[
#ifdef AURORA_ENABLE_GXCORE
namespace aurora::gfx {
PipelineRef gxcore::resident_pipeline_ref(const gxcore::PipelineConfig& config) {
  static thread_local uint32_t generation=0;
  static thread_local std::map<PipelineRef,PipelineRef> prepared;
  const auto current=pipeline_cache_generation();
  if(generation!=current){prepared.clear();generation=current;}
  const auto hash=xxh3_hash(config,static_cast<HashType>(ShaderType::GXCore));
  if(auto found=prepared.find(hash);found!=prepared.end())return found->second;
  const auto result=find_pipeline_impl(ShaderType::GXCore,config,
      [config]{return gxcore::create_pipeline(config);},PipelinePriority::Blocking);
  prepared.emplace(hash,result);return result;
}
}
#endif
]=])
