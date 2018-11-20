# typed: strict

extend T::Helpers

# This test file aims to exhaustively cover the cases for suggesting a sig on a
# child method, across all the possible builder methods that could be used on
# the parent:
#
# - no sig
# - standard sig
# - overridable
# - abstract
# - override
# - implementation

class ParentNoSig
  extend T::Helpers
  def foo; end
end
class ChildNoSig < ParentNoSig
  def foo; end
end

class ParentStandardSig
  extend T::Helpers
  sig {void}
  def foo; end
end
class ChildStandardSig < ParentStandardSig
  def foo; end
end

class ParentOverridable
  extend T::Helpers
  sig {overridable.void}
  def foo; end
end
class ChildOverridable < ParentOverridable
  def foo; end
end

class ParentAbstract
  extend T::Helpers
  abstract!
  sig {abstract.void}
  def foo; end
end
class ChildAbstract < ParentAbstract
  def foo; end
end

class GrandParentOverride
  # Only need this class to allow `override` below.
  extend T::Helpers
  sig {overridable.void}
  def foo; end
end
class ParentOverride < GrandParentOverride
  sig {override.void}
  def foo; end
end
class ChildOverride < ParentOverride
  def foo; end
end

class GrandParentImplementation
  # Only need this class to allow `implementation` below.
  extend T::Helpers
  abstract!
  sig {abstract.void}
  def foo; end
end
class ParentImplementation < GrandParentImplementation
  sig {implementation.void}
  def foo; end
end
class ChildImplementation < ParentImplementation
  def foo; end
end