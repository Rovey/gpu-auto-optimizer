import { handleDelete, rpcFromEnv } from "../_shared/telemetry.ts";

const rpc = rpcFromEnv();
Deno.serve((req) => handleDelete(req, rpc));
