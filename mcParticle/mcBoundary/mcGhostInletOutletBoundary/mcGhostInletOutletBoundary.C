/*---------------------------------------------------------------------------*\
                pdfFoam: General Purpose PDF Solution Algorithm
                   for Reactive Flow Simulations in OpenFOAM

 Copyright (C) 2012 Michael Wild, Heng Xiao, Patrick Jenny,
                    Institute of Fluid Dynamics, ETH Zurich
-------------------------------------------------------------------------------
License
    This file is part of pdfFoam.

    pdfFoam is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) version 3 of the same License.

    pdfFoam is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with pdfFoam.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "mcGhostInletOutletBoundary.H"

#include "addToRunTimeSelectionTable.H"
#include "mcParticle.H"
#include "mcParticleCloud.H"
#include "surfaceMesh.H"
#include "fvsPatchField.H"

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

namespace Foam
{

    defineTypeNameAndDebug(mcGhostInletOutletBoundary, 0);
    addNamedToRunTimeSelectionTable
    (
        mcBoundary,
        mcGhostInletOutletBoundary,
        mcBoundary,
        ghostInletOutlet
    );

    HashSet<label> mcGhostInletOutletBoundary::ghostCellHash_(256);
    bool mcGhostInletOutletBoundary::purgedGhosts_(false);

} // namespace Foam

// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::mcGhostInletOutletBoundary::mcGhostInletOutletBoundary
(
    mcParticleCloud& cloud,
    label patchID,
    const dictionary& dict
)
:
    mcOpenBoundary(cloud, patchID, dict)
{
    // FIXME: Is this correct?
    reflecting() = false;
    findGhostLayer();
}

// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

// Enforce in/out flow BCs by populating ghost cells
void Foam::mcGhostInletOutletBoundary::populateGhostCells()
{
    const fvMesh& mesh = cloud().mesh();
#ifdef FULLDEBUG
    if (debug > 1 && !statFile_.valid())
    {
        statFile_.reset
        (
            new OFstream
            (
                mesh.time().path()/patch().name()+".ghostInletOutlet"
            )
        );
    }
#endif
    label np = 0;
    label ng = 0;
    const label Npc = cloud().solutionDict().particlesPerCell();
    forAll(ghostCellLayer_, faceCelli)
    {
        ++ng;
        np += Npc;

        label celli = ghostCellLayer_[faceCelli];
        label patchI = patchID();
        scalar m = mesh.V()[celli]
                 * cloud().rhocPdf().boundaryField()[patchI][faceCelli]
                 / Npc;
        const vector& Updf = cloud().Ufv().boundaryField()[patchI][faceCelli];
        scalar urms = sqrt(
            2./3.*cloud().kfv()().boundaryField()[patchI][faceCelli]);
        vector uscales(urms, urms, urms);
        label  ghost = 1;
        vector shift = ghostCellShifts_[faceCelli];
        // Phi: from patch value (boundary condition)
        scalarField Phi(cloud().PhicPdf().size());
        forAll(Phi, PhiI)
        {
            const volScalarField& f = *(cloud().PhicPdf()[PhiI]);
            Phi[PhiI] = f.boundaryField()[patchID()][faceCelli];
        }

        // When compiling in Debug mode, generate each particle individually
#ifdef FULLDEBUG
        label N = 1;
        for (label i=0; i < Npc; ++i)
        {
#else
        label N = Npc;
#endif
        cloud().particleGenInCell
        (
            celli,
            N,
            m,
            Updf,
            uscales,
            Phi,
            shift,
            ghost
        );
#ifdef FULLDEBUG
        if (debug > 1)
        {
            const vector& u = cloud().last()->UParticle();
            const vector& Sf = mesh.Sf().boundaryField()[patchID()][faceCelli];
            if ((Sf & u) < 0.)
            {
                statFile_() << u.x() << tab << u.y() << tab << u.z() << nl;
            }
        }
        } // end for(i)
#endif
    }
#ifdef FULLDEBUG
    if (debug > 1)
    {
        statFile_().flush();
    }
#endif
    purgedGhosts_ = false;
    if (debug)
    {
        reduce(np, sumOp<label>());
        reduce(ng, sumOp<label>());
        Info<< "Ghost particles: " << np << " generated in " << ng
            << " ghost cells of patch " << patch().name() << endl;
    }
}


void Foam::mcGhostInletOutletBoundary::purgeGhostParticles()
{
    if (purgedGhosts_)
        return;
    const fvMesh& mesh = cloud().mesh();
    label nDelete = 0;
    label nAdmit = 0;
    forAllIter(mcParticleCloud, cloud(), pIter)
    {
        // only operate on ghost particles
        if(!pIter().ghost())
            continue;
        mcParticle & p = pIter();
        label celli = p.cell();
        if (ghostCellHash_.found(celli)) // still in ghost cell
        {
            cloud().deleteParticle(p);
            ++nDelete;
        }
        else
        {
            // shift and admit this particle as normal member
            p.position() -= p.shift(); // update position
            label newCelli = -1;
            label curCelli = p.cell();
            forAll(mesh.cellCells()[curCelli], nei)
            {
                label neiCellId = mesh.cellCells()[curCelli][nei];
                if (mesh.pointInCell(p.position(), neiCellId))
                {
                    newCelli = neiCellId;
                    break;
                }
            }
            // Not found in neighbour cells: global search
            if (newCelli < 0)
            {
                newCelli = mesh.findCell(p.position());
            }
            if (newCelli < 0)
            {
                WarningIn("mcGhostInletOutletBoundary::"
                    "purgeGhostParticles(mcParticleCloud&)")
                    << " Shifting of ghost particles caused loss."  << nl
                    << " Possible causes are: " << nl
                    << "  (1) Strange ghost cell shapes;" << nl
                    << "  (2) parallel computing + large time steps" << nl
                    << " Info for the lost particle: " << nl
                    << p.info() << endl;
                cloud().deleteParticle(p);
                ++nDelete;
            }
            p.cell()  = newCelli;
            p.ghost() = 0;
            p.shift() = vector::zero;
            ++nAdmit;
        }
    }
    purgedGhosts_ = true;
    if (debug)
    {
        reduce(nDelete, sumOp<label>());
        reduce(nAdmit, sumOp<label>());
        Info<< "Ghost particles: " << nDelete << " deleted, "
            << nAdmit << " admitted." << endl;
    }
}


void Foam::mcGhostInletOutletBoundary::findGhostLayer()
{
    label nf = patch().size();

    if (nf > 0)
    {
        ghostCellLayer_.setSize(nf);
        ghostCellShifts_.setSize(nf);
        const fvMesh& mesh = cloud().mesh();
        const cellList& cells = mesh.cells();
        const faceList& faces = mesh.faces();
        // Find the ghost cells and faces
        const polyPatch& pp = patch().patch();
        forAll(pp, facei)
        {
            // find ghost cell
            label faceCelli = pp.faceCells()[facei];
            ghostCellLayer_[facei] =  faceCelli;
            //- Add  to hash set
            ghostCellHash_.insert(faceCelli);
            // find opposite face
            label gFaceI = pp.start() + facei;
            const cell& ownCell = cells[faceCelli];
            label oppositeFaceI = ownCell.opposingFaceLabel(gFaceI, faces);
            if (oppositeFaceI == -1)
            {
                FatalErrorIn("mcGhostInletOutletBoundary::findGhostLayer()")
                    << "Face:" << facei << " owner cell:" << faceCelli
                    << " is not a hex?" << abort(FatalError);
            }
            else
            {
                ghostCellShifts_[facei] =
                      mesh.Cf()[oppositeFaceI]
                    - mesh.Cf().boundaryField()[patchID()][facei];
            }
        }
    }
}


void Foam::mcGhostInletOutletBoundary::correct(bool afterMove)
{
    if (afterMove)
    {
        purgeGhostParticles();
    }
    else
    {
        populateGhostCells();
    }
}

// ************************************************************************* //
