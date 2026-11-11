// node:crypto for the embedded runtime: the host provides crypto.randomUUID.
export const randomUUID = (): string => (globalThis as any).crypto.randomUUID();
