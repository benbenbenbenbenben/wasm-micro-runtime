(component
  (core module
    (memory (export "memory") 1)
    (global $heap (mut i32) (i32.const 64))
    (func $cabi_realloc (export "cabi_realloc") (param i32 i32 i32 i32) (result i32)
      (local $ptr i32)
      local.get 3
      i32.eqz
      if (result i32)
        i32.const 0
      else
        global.get $heap
        local.set $ptr
        global.get $heap
        local.get 3
        i32.add
        global.set $heap
        local.get $ptr
      end)
    (func $nested-core (export "nested-core") (param i32 i32) (result i32)
      local.get 1)
    (func $mixed-core (export "mixed-core") (param i32 i32 i32) (result i32)
      local.get 0
      local.get 2
      i32.add))
  (core instance $i (instantiate 0))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "cabi_realloc" (core func $realloc-core))
  (alias core export $i "nested-core" (core func $nested-core))
  (alias core export $i "mixed-core" (core func $mixed-core))
  (type $string-tuple (tuple string))
  (type $nested-record (record (field "msg" $string-tuple)))
  (type $nested-func (func (param "input" $nested-record) (result s32)))
  (func $nested-param (type $nested-func)
    (canon lift (core func $nested-core) (memory $mem) (realloc $realloc-core)
      string-encoding=utf8))
  (type $mixed-record (record (field "code" s32) (field "payload" $string-tuple)))
  (type $mixed-func (func (param "input" $mixed-record) (result s32)))
  (func $mixed-param (type $mixed-func)
    (canon lift (core func $mixed-core) (memory $mem) (realloc $realloc-core)
      string-encoding=utf8))
  (export "nested-param" (func $nested-param))
  (export "mixed-param" (func $mixed-param)))
