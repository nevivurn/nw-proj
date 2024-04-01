{ stdenv }:

stdenv.mkDerivation (finalAttrs: {
  name = "nw-proj1";
  src = ./.;

  #nativeBuildInputs = [ pandoc texliveSmall ];
  #nativeCheckInputs = [ go_1_22 ];

  enableParallelBuilding = true;

  #doCheck = true;
  #checkPhase = ''
  #  runHook preCheck
  #  GOCACHE=$(mktemp -d) make check
  #  runHook postCheck
  #'';

  installPhase = ''
    runHook preInstall
    install -Dm755 sserver $out/bin/sserver
    runHook postInstall
  '';
})
