# typed: true

class Nilable < T::Struct
  prop :foo, T.nilable(Integer)
end

T.reveal_type(Nilable.new.foo) # error: Revealed type: `T.nilable(Integer)`
Nilable.new(foo: "no") # error: `String("no")` does not match `T.nilable(Integer)` for argument `foo`
Nilable.new(foo: 3, bar: 4) # error: Unrecognized keyword argument `bar` passed for method `Nilable#initialize`
T.reveal_type(Nilable.new(foo: 3).foo) # error: Revealed type: `T.nilable(Integer)`