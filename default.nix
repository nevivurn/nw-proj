{ stdenv, go_1_22 }:

stdenv.mkDerivation (finalAttrs: {
  name = "nw-proj2";
  src = ./.;

  nativeCheckInputs = [ go_1_22 ];

  enableParallelBuilding = true;
  enableParallelChecking = true;

  doCheck = true;
  preCheck = ''
    export GOCACHE=$(mktemp -d)
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 shttpd $out/bin/shttpd
    runHook postInstall
  '';

  meta.mainProgram = "shttpd";
})
