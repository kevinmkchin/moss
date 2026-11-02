-- moss: tells moss the moment cmd takes a command line (OSC 7777;run), as the prompt (OSC 9;9) tells it the command
-- has ended. moss counts a shell as running something from then until its prompt is back: its file browser is read-only
-- meanwhile. Enter alone isn't enough: a line typed ahead while a command runs is taken once the prompt comes back,
-- with no Enter after it. Shipped next to moss.exe and loaded through CLINK_PATH, so every moss has the current one.

clink.onendedit(function(line)
    if line and line:find("%S") then clink.print("\x1b]7777;run\x1b\\", NONL) end
end)
