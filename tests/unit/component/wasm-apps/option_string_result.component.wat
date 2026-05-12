(component
  (type $opt-string (option string))
  (type $f (func (result $opt-string)))
  (core module $m
    (memory (export "memory") 1)
    (global $heap (mut i32) (i32.const 512))
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
    (func (export "f") (result i32)
      i32.const 12
      call $cabi_realloc
      local.tee 0
      i32.const 0
      i32.store
      local.get 0))
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "cabi_realloc" (core func $realloc))
  (alias core export $i "f" (core func $f_func))
  (func $g (type $f) (canon lift (core func $f_func) (memory $mem) (realloc $realloc) string-encoding=utf8))
  (export "g" (func $g))
)
