(module
  (global $sum (mut i32) (i32.const 0))

  (func (export "resource-dtor") (param i32)
    global.get $sum
    local.get 0
    i32.add
    global.set $sum)

  (func (export "read-dtor-sum") (result i32)
    global.get $sum))
