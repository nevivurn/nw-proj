{ stdenv, go_1_22, pandoc, texliveSmall }:

stdenv.mkDerivation (finalAttrs: {
  name = "nw-proj1";
  src = ./work;

  nativeBuildInputs = [ pandoc texliveSmall ];
  nativeCheckInputs = [ go_1_22 ];

  enableParallelBuilding = true;

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    GOCACHE=$(mktemp -d) make check
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 client $out/bin/client
    install -Dm755 server $out/bin/server
    install -Dm644 readme.pdf $out/share/doc/nw-proj1/readme.pdf
    runHook postInstall
  '';
})
