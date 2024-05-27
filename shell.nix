{ mkShell, pandoc, texliveSmall, nw-proj, gdb, valgrind }:

mkShell {
  inputsFrom = [ nw-proj ];
  nativeBuildInputs = [ gdb valgrind pandoc texliveSmall ];
}
