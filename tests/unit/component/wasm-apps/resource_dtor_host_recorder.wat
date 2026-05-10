(module
  (import "env" "record_dtor" (func $record_dtor (param i32)))

  (func (export "resource-dtor") (param i32)
    local.get 0
    call $record_dtor))
