/*
 * Copyright 2013 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrGeometryProcessor_DEFINED
#define GrGeometryProcessor_DEFINED

#include "GrColor.h"
#include "GrProcessor.h"
#include "GrShaderVar.h"

/*
 * The GrPrimitiveProcessor represents some kind of geometric primitive.  This includes the shape
 * of the primitive and the inherent color of the primitive.  The GrPrimitiveProcessor is
 * responsible for providing a color and coverage input into the Ganesh rendering pipeline.  Through
 * optimization, Ganesh may decide a different color, no color, and / or no coverage are required
 * from the GrPrimitiveProcessor, so the GrPrimitiveProcessor must be able to support this
 * functionality.  We also use the GrPrimitiveProcessor to make batching decisions.
 *
 * There are two feedback loops between the GrFragmentProcessors, the GrXferProcessor, and the
 * GrPrimitiveProcessor.  These loops run on the CPU and compute any invariant components which
 * might be useful for correctness / optimization decisions.  The GrPrimitiveProcessor seeds these
 * loops, one with initial color and one with initial coverage, in its
 * onComputeInvariantColor / Coverage calls.  These seed values are processed by the subsequent
 * stages of the rendering pipeline and the output is then fed back into the GrPrimitiveProcessor in
 * the initBatchTracker call, where the GrPrimitiveProcessor can then initialize the GrBatchTracker
 * struct with the appropriate values.
 *
 * We are evolving this system to move towards generating geometric meshes and their associated
 * vertex data after we have batched and reordered draws.  This system, known as 'deferred geometry'
 * will allow the GrPrimitiveProcessor much greater control over how data is transmitted to shaders.
 *
 * In a deferred geometry world, the GrPrimitiveProcessor can always 'batch'  To do this, each
 * primitive type is associated with one GrPrimitiveProcessor, who has complete control of how
 * it draws.  Each primitive draw will bundle all required data to perform the draw, and these
 * bundles of data will be owned by an instance of the associated GrPrimitiveProcessor.  Bundles
 * can be updated alongside the GrBatchTracker struct itself, ultimately allowing the
 * GrPrimitiveProcessor complete control of how it gets data into the fragment shader as long as
 * it emits the appropriate color, or none at all, as directed.
 */

/*
 * A struct for tracking batching decisions.  While this lives on GrOptState, it is managed
 * entirely by the derived classes of the GP.
 * // TODO this was an early attempt at handling out of order batching.  It should be
 * used carefully as it is being replaced by GrBatch
 */
class GrBatchTracker {
public:
    template <typename T> const T& cast() const {
        SkASSERT(sizeof(T) <= kMaxSize);
        return *reinterpret_cast<const T*>(fData.get());
    }

    template <typename T> T* cast() {
        SkASSERT(sizeof(T) <= kMaxSize);
        return reinterpret_cast<T*>(fData.get());
    }

    static const size_t kMaxSize = 32;

private:
    SkAlignedSStorage<kMaxSize> fData;
};

class GrIndexBufferAllocPool;
class GrGLCaps;
class GrGLPrimitiveProcessor;
class GrVertexBufferAllocPool;

struct GrInitInvariantOutput;

/*
 * This struct allows the GrPipeline to communicate information about the pipeline.  Most of this
 * is overrides, but some of it is general information.  Logically it should live in GrPipeline.h,
 * but this is problematic due to circular dependencies.
 */
struct GrPipelineInfo {
    bool fColorIgnored;
    bool fCoverageIgnored;
    GrColor fOverrideColor;
    bool fUsesLocalCoords;
};

/*
 * This enum is shared by GrPrimitiveProcessors and GrGLPrimitiveProcessors to coordinate shaders
 * with vertex attributes / uniforms.
 */
enum GrGPInput {
    kAllOnes_GrGPInput,
    kAttribute_GrGPInput,
    kUniform_GrGPInput,
    kIgnored_GrGPInput,
};

/*
 * GrPrimitiveProcessor defines an interface which all subclasses must implement.  All
 * GrPrimitiveProcessors must proivide seed color and coverage for the Ganesh color / coverage
 * pipelines, and they must provide some notion of equality
 */
class GrPrimitiveProcessor : public GrProcessor {
public:
    // TODO let the PrimProc itself set this in its setData call, this should really live on the
    // bundle of primitive data
    const SkMatrix& viewMatrix() const { return fViewMatrix; }
    const SkMatrix& localMatrix() const { return fLocalMatrix; }

    virtual void initBatchTracker(GrBatchTracker*, const GrPipelineInfo&) const = 0;

    virtual bool canMakeEqual(const GrBatchTracker& mine,
                              const GrPrimitiveProcessor& that,
                              const GrBatchTracker& theirs) const = 0;

    virtual void getInvariantOutputColor(GrInitInvariantOutput* out) const = 0;
    virtual void getInvariantOutputCoverage(GrInitInvariantOutput* out) const = 0;

    // Only the GrGeometryProcessor subclass actually has a geo shader or vertex attributes, but
    // we put these calls on the base class to prevent having to cast
    virtual bool willUseGeoShader() const = 0;

    /*
     * This is a safeguard to prevent GrPrimitiveProcessor's from going beyond platform specific
     * attribute limits. This number can almost certainly be raised if required.
     */
    static const int kMaxVertexAttribs = 6;

    struct Attribute {
        Attribute()
            : fName(NULL)
            , fType(kFloat_GrVertexAttribType)
            , fOffset(0) {}
        Attribute(const char* name, GrVertexAttribType type)
            : fName(name)
            , fType(type)
            , fOffset(SkAlign4(GrVertexAttribTypeSize(type))) {}
        const char* fName;
        GrVertexAttribType fType;
        size_t fOffset;
    };

    int numAttribs() const { return fNumAttribs; }
    const Attribute& getAttrib(int index) const {
        SkASSERT(index < fNumAttribs);
        return fAttribs[index];
    }

    // Returns the vertex stride of the GP.  A common use case is to request geometry from a
    // drawtarget based off of the stride, and to populate this memory using an implicit array of
    // structs.  In this case, it is best to assert the vertexstride == sizeof(VertexStruct).
    size_t getVertexStride() const { return fVertexStride; }

    /**
     * Gets a transformKey from an array of coord transforms
     */
    uint32_t getTransformKey(const SkTArray<const GrCoordTransform*, true>&) const;

    /**
     * Sets a unique key on the GrProcessorKeyBuilder that is directly associated with this geometry
     * processor's GL backend implementation.
     */
    virtual void getGLProcessorKey(const GrBatchTracker& bt,
                                   const GrGLCaps& caps,
                                   GrProcessorKeyBuilder* b) const = 0;


    /** Returns a new instance of the appropriate *GL* implementation class
        for the given GrProcessor; caller is responsible for deleting
        the object. */
    virtual GrGLPrimitiveProcessor* createGLInstance(const GrBatchTracker& bt,
                                                     const GrGLCaps& caps) const = 0;

    bool isPathRendering() const { return fIsPathRendering; }

protected:
    GrPrimitiveProcessor(const SkMatrix& viewMatrix, const SkMatrix& localMatrix,
                         bool isPathRendering)
        : fNumAttribs(0)
        , fVertexStride(0)
        , fViewMatrix(viewMatrix)
        , fLocalMatrix(localMatrix)
        , fIsPathRendering(isPathRendering) {}

    /*
     * CanCombineOutput will return true if two draws are 'batchable' from a color perspective.
     * TODO remove this when GPs can upgrade to attribute color
     */
    static bool CanCombineOutput(GrGPInput left, GrColor lColor, GrGPInput right, GrColor rColor) {
        if (left != right) {
            return false;
        }

        if (kUniform_GrGPInput == left && lColor != rColor) {
            return false;
        }

        return true;
    }

    static bool CanCombineLocalMatrices(const GrPrimitiveProcessor& left,
                                        bool leftUsesLocalCoords,
                                        const GrPrimitiveProcessor& right,
                                        bool rightUsesLocalCoords) {
        if (leftUsesLocalCoords != rightUsesLocalCoords) {
            return false;
        }

        if (leftUsesLocalCoords && !left.localMatrix().cheapEqualTo(right.localMatrix())) {
            return false;
        }
        return true;
    }

    Attribute fAttribs[kMaxVertexAttribs];
    int fNumAttribs;
    size_t fVertexStride;

private:
    virtual bool hasExplicitLocalCoords() const = 0;

    const SkMatrix fViewMatrix;
    SkMatrix fLocalMatrix;
    bool fIsPathRendering;

    typedef GrProcessor INHERITED;
};

/**
 * A GrGeometryProcessor is a flexible method for rendering a primitive.  The GrGeometryProcessor
 * has complete control over vertex attributes and uniforms(aside from the render target) but it
 * must obey the same contract as any GrPrimitiveProcessor, specifically it must emit a color and
 * coverage into the fragment shader.  Where this color and coverage come from is completely the
 * responsibility of the GrGeometryProcessor.
 */
class GrGeometryProcessor : public GrPrimitiveProcessor {
public:
    // TODO the Hint can be handled in a much more clean way when we have deferred geometry or
    // atleast bundles
    GrGeometryProcessor(GrColor color,
                        const SkMatrix& viewMatrix = SkMatrix::I(),
                        const SkMatrix& localMatrix = SkMatrix::I(),
                        bool opaqueVertexColors = false)
        : INHERITED(viewMatrix, localMatrix, false)
        , fColor(color)
        , fOpaqueVertexColors(opaqueVertexColors)
        , fWillUseGeoShader(false)
        , fHasVertexColor(false)
        , fHasLocalCoords(false) {}

    bool willUseGeoShader() const { return fWillUseGeoShader; }

    /*
     * In an ideal world, two GrGeometryProcessors with the same class id and texture accesses
     * would ALWAYS be able to batch together.  If two GrGeometryProcesosrs are the same then we
     * will only keep one of them.  The remaining GrGeometryProcessor then updates its
     * GrBatchTracker to incorporate the draw information from the GrGeometryProcessor we discard.
     * Any bundles associated with the discarded GrGeometryProcessor will be attached to the
     * remaining GrGeometryProcessor.
     */
    bool canMakeEqual(const GrBatchTracker& mine,
                      const GrPrimitiveProcessor& that,
                      const GrBatchTracker& theirs) const SK_OVERRIDE {
        if (this->classID() != that.classID() || !this->hasSameTextureAccesses(that)) {
            return false;
        }

        // TODO let the GPs decide this
        if (!this->viewMatrix().cheapEqualTo(that.viewMatrix())) {
            return false;
        }

        // TODO remove the hint
        const GrGeometryProcessor& other = that.cast<GrGeometryProcessor>();
        if (fHasVertexColor && fOpaqueVertexColors != other.fOpaqueVertexColors) {
            return false;
        }

        // TODO this equality test should really be broken up, some of this can live on the batch
        // tracker test and some of this should be in bundles
        if (!this->onIsEqual(other)) {
            return false;
        }

        return this->onCanMakeEqual(mine, other, theirs);
    }
    
    // TODO we can remove color from the GrGeometryProcessor base class once we have bundles of
    // primitive data
    GrColor color() const { return fColor; }

    // TODO this is a total hack until the gp can do deferred geometry
    bool hasVertexColor() const { return fHasVertexColor; }

    void getInvariantOutputColor(GrInitInvariantOutput* out) const SK_OVERRIDE;
    void getInvariantOutputCoverage(GrInitInvariantOutput* out) const SK_OVERRIDE;

protected:
    /*
     * An optional simple helper function to determine by what means the GrGeometryProcessor should
     * use to provide color.  If we are given an override color(ie the given overridecolor is NOT
     * GrColor_ILLEGAL) then we must always emit that color(currently overrides are only supported
     * via uniform, but with deferred Geometry we could use attributes).  Otherwise, if our color is
     * ignored then we should not emit a color.  Lastly, if we don't have vertex colors then we must
     * emit a color via uniform
     * TODO this function changes quite a bit with deferred geometry.  There the GrGeometryProcessor
     * can upload a new color via attribute if needed.
     */
    static GrGPInput GetColorInputType(GrColor* color, GrColor primitiveColor,
                                       const GrPipelineInfo& init,
                                       bool hasVertexColor) {
        if (init.fColorIgnored) {
            *color = GrColor_ILLEGAL;
            return kIgnored_GrGPInput;
        } else if (GrColor_ILLEGAL != init.fOverrideColor) {
            *color = init.fOverrideColor;
            return kUniform_GrGPInput;
        }

        *color = primitiveColor;
        if (hasVertexColor) {
            return kAttribute_GrGPInput;
        } else {
            return kUniform_GrGPInput;
        }
    }

    /**
     * Subclasses call this from their constructor to register vertex attributes.  Attributes
     * will be padded to the nearest 4 bytes for performance reasons.
     * TODO After deferred geometry, we should do all of this inline in GenerateGeometry alongside
     * the struct used to actually populate the attributes.  This is all extremely fragile, vertex
     * attributes have to be added in the order they will appear in the struct which maps memory.
     * The processor key should reflect the vertex attributes, or there lack thereof in the
     * GrGeometryProcessor.
     */
    const Attribute& addVertexAttrib(const Attribute& attribute) {
        SkASSERT(fNumAttribs < kMaxVertexAttribs);
        fVertexStride += attribute.fOffset;
        fAttribs[fNumAttribs] = attribute;
        return fAttribs[fNumAttribs++];
    }

    void setWillUseGeoShader() { fWillUseGeoShader = true; }

    // TODO hack see above
    void setHasVertexColor() { fHasVertexColor = true; }
    void setHasLocalCoords() { fHasLocalCoords = true; }

    virtual void onGetInvariantOutputColor(GrInitInvariantOutput*) const {}
    virtual void onGetInvariantOutputCoverage(GrInitInvariantOutput*) const = 0;

private:
    virtual bool onCanMakeEqual(const GrBatchTracker& mine,
                                const GrGeometryProcessor& that,
                                const GrBatchTracker& theirs) const = 0;

    // TODO delete this when we have more advanced equality testing via bundles and the BT
    virtual bool onIsEqual(const GrGeometryProcessor&) const = 0;

    bool hasExplicitLocalCoords() const SK_OVERRIDE { return fHasLocalCoords; }

    GrColor fColor;
    bool fOpaqueVertexColors;
    bool fWillUseGeoShader;
    bool fHasVertexColor;
    bool fHasLocalCoords;

    typedef GrPrimitiveProcessor INHERITED;
};

/*
 * The path equivalent of the GP.  For now this just manages color. In the long term we plan on
 * extending this class to handle all nvpr uniform / varying / program work.
 */
class GrPathProcessor : public GrPrimitiveProcessor {
public:
    static GrPathProcessor* Create(GrColor color,
                                   const SkMatrix& viewMatrix = SkMatrix::I(),
                                   const SkMatrix& localMatrix = SkMatrix::I()) {
        return SkNEW_ARGS(GrPathProcessor, (color, viewMatrix, localMatrix));
    }
    
    void initBatchTracker(GrBatchTracker*, const GrPipelineInfo&) const SK_OVERRIDE;

    bool canMakeEqual(const GrBatchTracker& mine,
                      const GrPrimitiveProcessor& that,
                      const GrBatchTracker& theirs) const SK_OVERRIDE;

    const char* name() const SK_OVERRIDE { return "PathProcessor"; }

    GrColor color() const { return fColor; }

    void getInvariantOutputColor(GrInitInvariantOutput* out) const SK_OVERRIDE;
    void getInvariantOutputCoverage(GrInitInvariantOutput* out) const SK_OVERRIDE;

    bool willUseGeoShader() const SK_OVERRIDE { return false; }

    virtual void getGLProcessorKey(const GrBatchTracker& bt,
                                   const GrGLCaps& caps,
                                   GrProcessorKeyBuilder* b) const SK_OVERRIDE;

    virtual GrGLPrimitiveProcessor* createGLInstance(const GrBatchTracker& bt,
                                                     const GrGLCaps& caps) const SK_OVERRIDE;

protected:
    GrPathProcessor(GrColor color, const SkMatrix& viewMatrix, const SkMatrix& localMatrix);

private:
    bool hasExplicitLocalCoords() const SK_OVERRIDE { return false; }

    GrColor fColor;

    typedef GrPrimitiveProcessor INHERITED;
};

#endif
