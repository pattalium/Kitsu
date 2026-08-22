export async function validateHealthResponse(response) {
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  const body = (await response.text()).trim();
  if (body !== "ok") throw new Error("unexpected health response");
}
