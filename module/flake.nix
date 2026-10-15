{
  description = "storage_gateway: the storage_module API, served by a remote Logos Storage node's REST API";

  inputs = {
    # Basecamp 0.3.x: builder 0.3.1 (release tag).
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
