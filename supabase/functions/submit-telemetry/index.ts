import { handleSubmit, rpcFromEnv } from "../_shared/telemetry.ts";

const rpc = rpcFromEnv();
Deno.serve((req) => handleSubmit(req, rpc));
