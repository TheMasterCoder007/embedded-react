// Platform — minimal RN analog. OS is "embedded"; select() picks the embedded entry (falling back
// to default) so apps can write Platform.select({ embedded: ..., default: ... }).
export const Platform = {
  OS: 'embedded',
  select(specifics) {
    if (specifics == null) return undefined;
    if ('embedded' in specifics) return specifics.embedded;
    return specifics.default;
  },
};
