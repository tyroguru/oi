# cmake -DFEATURES=a,b -DBASE=... -DEXTRA=... -DOUT=... -P ConcatConfig.cmake
# Writes `features = [...]` (top-level keys must precede the base config's
# tables), then the base config, then an optional extra TOML fragment.
set(_out "")
if(FEATURES)
  string(REPLACE "," ";" FEATURES "${FEATURES}")
  list(TRANSFORM FEATURES PREPEND "\"")
  list(TRANSFORM FEATURES APPEND "\"")
  list(JOIN FEATURES ", " _joined)
  string(APPEND _out "features = [${_joined}]\n")
endif()
file(READ "${BASE}" _base)
string(APPEND _out "${_base}")
if(EXTRA)
  file(READ "${EXTRA}" _extra)
  string(APPEND _out "\n${_extra}")
endif()
file(WRITE "${OUT}" "${_out}")
