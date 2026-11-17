# Changelog

Notable changes to the libraries and the demo app. Versions follow [semantic versioning](https://semver.org);
the PS5 catalog is versioned separately by its `catalogId` (frozen at the `catalog-v1` tag).

## 1.0.0

The first public release.

- **A2UI renderer** (`renderer/`, CMake `a2ui::a2ui`): A2UI 1.0 surfaces on the PS5 catalog, focus
  navigation, dialogs, actions and the portable haptics model. Consumable with `find_package` or
  FetchContent; a minimal consumer is in `renderer/examples/hello`.
- **Agent runtime** (`runtime/`, npm `ps5-a2ui-agent`): `AgentThread` and `serveAgent`, intent
  tools and their translation to A2UI, the catalog validator, an approval gate, secret handles,
  client tools, and any OpenAI-compatible model.
- **CopilotKit Channels adapter** (`channel/`, npm `copilotkit-channels-ps5`): the PS5 as a
  channel, with Channels UI rendered to the PS5 catalog and notifications while the app is closed.
- **PS5 catalog** v1 (`catalog/ps5.catalog.json`), frozen.
- **PS5 A2UI** demo app: an on-console agent in QuickJS, key setup from a phone, voice input with
  on-console speech-to-text, a home-screen tile and background presence.
