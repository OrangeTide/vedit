-- Rewrite links between manual pages to their rendered form.
--
-- The site is flat: every docs/<name>.md becomes <name>.html beside the
-- others, so "docs/07-vi.md", "./07-vi.md" and "07-vi.md" all become
-- "07-vi.html". External links and bare anchors are left alone.

function Link(el)
    local target = el.target
    if target:match("^%a[%w+.-]*://") or target:match("^#") or
       target:match("^mailto:") then
        return nil
    end
    local path, anchor = target:match("^([^#]*)(#?.*)$")
    if not path:match("%.md$") then
        return nil
    end
    path = path:gsub("^%./", ""):gsub("^docs/", "")
    if path:match("/") then
        return nil          -- outside the manual
    end
    el.target = path:gsub("%.md$", ".html") .. anchor
    return el
end
