{ stdenv, pandoc, texliveSmall, go_1_22 }:

stdenv.mkDerivation (finalAttrs: {
  name = "nw-proj2";
  src = ./.;

  nativeBuildInputs = [ pandoc texliveSmall ];
  nativeCheckInputs = [ go_1_22 ];

  doCheck = true;
  enableParallelChecking = true;
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
