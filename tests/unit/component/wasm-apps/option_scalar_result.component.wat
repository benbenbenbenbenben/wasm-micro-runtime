(component
  (type $opt-u32 (option u32))
  (type $f-none-type (func (result $opt-u32)))
  (type $f-some-type (func (result $opt-u32)))
  (core module $m
    (func (export "f_none") (result i32 i32)
      i32.const 0
      i32.const 0)
    (func (export "f_some") (result i32 i32)
      i32.const 1
      i32.const 42)
  )
  (core instance $i (instantiate $m))
  (alias core export $i "f_none" (core func $f_none))
  (alias core export $i "f_some" (core func $f_some))
  (func $g_none (type $f-none-type)
    (canon lift (core func $f_none))
  )
  (func $g_some (type $f-some-type)
    (canon lift (core func $f_some))
  )
  (export "g_none" (func $g_none))
  (export "g_some" (func $g_some))
)
