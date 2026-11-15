return function(reference_minor, weekend, premium_bp)
  if not weekend then return reference_minor end
  return reference_minor + (reference_minor * premium_bp + 5000) // 10000
end
