#pragma once

/**
 * @brief GPU-driven grass culling: frustum-culls grass blade clumps on the GPU and draws only survivors
 *        via DrawIndexedInstancedIndirect.
 *
 * Skyrim draws grass as hardware-instanced batches (BSMultiStreamInstanceTriShape) through a single choke
 * point (fDrawGrass @ 0x140D6C1E0): one DrawIndexedInstanced per (grass-type x land-patch) batch, replicating
 * a shared blade mesh across a static, per-cell instance stream (32 bytes/instance = 4x R16G16B16A16_FLOAT =
 * InstanceData1..4). Today there is NO per-blade view cull -- once a batch's AABB is visible, ALL its baked
 * blades draw, including everything behind/beside the camera.
 *
 * This fix detours fDrawGrass, mirrors the immutable engine instance stream into an SRV-readable buffer
 * (cached per cell), runs a compute pass (GrassCullCS) that tests each clump against the 6 frustum planes of
 * the CURRENTLY BOUND pass WorldViewProj (correct for the main color pass AND every shadow cascade), compacts
 * survivors verbatim into a vertex-buffer-capable UAV, and reissues the draw as DrawIndexedInstancedIndirect
 * with the survivor count kept on the GPU (no readback). The compacted stream keeps the identical FP16 layout,
 * so the engine input layout and the grass VS/PS are unchanged -- every surviving blade is byte-identical.
 *
 * SE-only (every RE'd offset is 1.5.97). Runtime-toggleable for A/B.
 */
struct GrassCull : EngineFix
{
	/** @brief Returns the human-readable name of this fix. */
	std::string GetName() override { return "Grass Cull"; }

	/** @brief Compiles the cull shader lazily and installs the fDrawGrass detour. */
	void Install() override;
};
