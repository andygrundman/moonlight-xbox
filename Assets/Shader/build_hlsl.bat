fxc /T vs_4_0_level_9_3 /Fo d3d11_vertex.fxc d3d11_vertex.hlsl
fxc /T ps_4_0 /Fo d3d11_yuv420_pixel_array.fxc d3d11_yuv420_pixel_array.hlsl

fxc /T ps_4_0_level_9_3 /Fo d3d11_pyrowave_pixel.fxc d3d11_pyrowave_pixel.hlsl

rem PyroWave compute shaders
rem idwt_sm50.hlsl / dequant_sm50.hlsl are generated from GLSL on the Mac side:
rem   glslc -fshader-stage=comp --target-env=vulkan1.1 -DSTORAGE_MODE=1 [-DSUBGROUP_EMULATION=1] -I ..\..\third_party\pyrowave\shaders <src>.comp -o <src>.spv
rem   spirv-cross --hlsl --shader-model 50 <src>.spv --output <dst>.hlsl
rem dequant_sm50 comes from glsl\wavelet_dequant.comp (SUBGROUP_EMULATION=1, STORAGE_MODE=1)
rem idwt_sm50 comes from glsl\idwt.comp (OUTPUT_LAYERED=1, FP16=0)
fxc /T cs_5_0 /Fo idwt_sm50.fxc idwt_sm50.hlsl
rem DCShift=true variant: used when an iDWT dispatch writes a final output plane
fxc /T cs_5_0 /D SPIRV_CROSS_CONSTANT_ID_0=true /Fo idwt_dc_sm50.fxc idwt_sm50.hlsl
fxc /T cs_5_0 /Fo dequant_sm50.fxc dequant_sm50.hlsl
