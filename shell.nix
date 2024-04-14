{ mkShell, nw-proj, gdb, valgrind }:

mkShell {
  inputsFrom = [ nw-proj ];
  nativeBuildInputs = [ gdb valgrind ];
}
