-- SPDX-License-Identifier: MIT
-- Every measured response is validated; this callback adds client CPU overhead.
local threads = {}
function setup(thread)
  table.insert(threads, thread)
end
function init(args)
  checked = 0
  invalid = 0
end
function response(status, headers, body)
  checked = checked + 1
  if status ~= 200 or body ~= "Hello World!" then
    invalid = invalid + 1
  end
end
function done(summary, latency, requests)
  local count, errors = 0, 0
  for _, thread in ipairs(threads) do
    count = count + thread:get("checked")
    errors = errors + thread:get("invalid")
  end
  io.write(string.format("KH_VALIDATED checked=%d invalid=%d\n", count, errors))
end
