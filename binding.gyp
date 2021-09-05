{
  'targets': [
    {
      'target_name': 'memif-native',
      'sources': [ 'src/memif.cc' ],
      'include_dirs': ["<!@(node -p \"require('node-addon-api').include\")"],
      'dependencies': ["<!(node -p \"require('node-addon-api').gyp\")"],
      'cflags!': [ '-fno-exceptions' ],
      'cflags_cc!': [ '-fno-exceptions' ],
      'link_settings': {
        'libraries': [ '-L/usr/local/lib', '-lmemif' ],
      }
    }
  ]
}