subdir('interp')

libdsp = static_library(
  'dsp',
  files(['dsp.c', 'dsp_dma.c']) + genh,
)

dsp = declare_dependency(
  objects: [
    libdsp.extract_all_objects(recursive: false),
    libdsp_cpu_interp.extract_all_objects(recursive: false),
  ],
)

mcpx_ss.add(dsp, files('gp_ep.c'))
