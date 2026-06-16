function(gavel_tool name src)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE gavel_core)
endfunction()

gavel_tool(gavel-replay gavel_replay.cpp)
gavel_tool(gavel-extract gavel_extract.cpp)
gavel_tool(gavel-fidelity gavel_fidelity.cpp)
gavel_tool(gavel-sim gavel_sim.cpp)
