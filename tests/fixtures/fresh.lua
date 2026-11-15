seen = (seen or 0) + 1
return function(reference, weekend, premium)
  return reference + seen
end
