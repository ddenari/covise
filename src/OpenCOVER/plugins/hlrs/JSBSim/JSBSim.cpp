/* This file is part of COVISE.

   You can use it under the terms of the GNU Lesser General Public License
   version 2.1 or later, see lgpl-2.1.txt.

 * License: LGPL 2+ */

//
//

#include "JSBSim.h"
#include "models/FGFCS.h"
#include "FGJSBBase.h"
#include "initialization/FGInitialCondition.h"
#include "models/FGModel.h"
#include "models/FGMassBalance.h"
#include "models/FGPropagate.h"
#include "models/FGInertial.h"
#include "math/FGLocation.h"
#include "models/FGAccelerations.h"
#include "models/FGPropulsion.h"
#include "models/FGAerodynamics.h"
#include "models/FGAircraft.h"
#include "models/propulsion/FGEngine.h"
#include "models/propulsion/FGPiston.h"
#include "cover/VRSceneGraph.h"
#include "cover/coVRCollaboration.h"
#include <UDPComm.h>

JSBSimPlugin *JSBSimPlugin::plugin = NULL;

JSBSimPlugin::JSBSimPlugin(): ui::Owner("JSBSimPlugin", cover->ui)
{
    fprintf(stderr, "JSBSimPlugin::JSBSimPlugin\n");
#if defined(_MSC_VER)
    // _clearfp();
    // _controlfp(_controlfp(0, 0) & ~(_EM_INVALID | _EM_ZERODIVIDE | _EM_OVERFLOW),
    //     _MCW_EM);
#elif defined(__GNUC__) && !defined(sgi) && !defined(__APPLE__)
    feenableexcept(FE_DIVBYZERO | FE_INVALID);
#endif

    plugin = this;
}

// this is called if the plugin is removed at runtime
JSBSimPlugin::~JSBSimPlugin()
{
    fprintf(stderr, "JSBSimPlugin::~JSBSimPlugin\n");

    delete FDMExec;

}

bool JSBSimPlugin::init()
{

    JSBMenu = new ui::Menu("JSBSim", this);

    printCatalog = new ui::Action(JSBMenu, "printCatalog");
    printCatalog->setCallback([this]() {
        if (FDMExec)
            FDMExec->PrintPropertyCatalog();
    });
    pauseButton = new ui::Button(JSBMenu, "pause");
    pauseButton->setState(false);
    pauseButton->setCallback([this](bool state) {
        if (FDMExec)
        {
            if (state)
                FDMExec->Hold();
            else
                FDMExec->Resume();
        }
    });

    // *** SET UP JSBSIM *** //
    FDMExec = new JSBSim::FGFDMExec();
    FDMExec->SetRootDir(RootDir);
    FDMExec->SetAircraftPath(SGPath("aircraft"));
    FDMExec->SetEnginePath(SGPath("engine"));
    FDMExec->SetSystemsPath(SGPath("systems"));
    FDMExec->GetPropertyManager()->Tie("simulation/frame_start_time", &actual_elapsed_time);
    FDMExec->GetPropertyManager()->Tie("simulation/cycle_duration", &cycle_duration);


    Atmosphere = FDMExec->GetAtmosphere();
    Winds = FDMExec->GetWinds();
    FCS = FDMExec->GetFCS();
    MassBalance = FDMExec->GetMassBalance();
    Propulsion = FDMExec->GetPropulsion();
    Aircraft = FDMExec->GetAircraft();
    Propagate = FDMExec->GetPropagate();
    Auxiliary = FDMExec->GetAuxiliary();
    Inertial = FDMExec->GetInertial();
    Aerodynamics = FDMExec->GetAerodynamics();
    GroundReactions = FDMExec->GetGroundReactions();
    Accelerations = FDMExec->GetAccelerations();

    if (nohighlight) FDMExec->disableHighLighting();
    FDMExec->Setdt(1.0 / 120.0);

    bool result = false;

    std::string line = coCoviseConfig::getEntry("COVER.Plugin.JSBSim.ScriptName");
    SGPath ScriptName;
    ScriptName.set(line);
    std::string AircraftDir = coCoviseConfig::getEntry("aircraftDir", "COVER.Plugin.JSBSim.Model","D:/src/gitbase/jsbsim/aircraft");
    std::string AircraftName = coCoviseConfig::getEntry("aircraft", "COVER.Plugin.JSBSim.Model","paraglider");
    std::string EnginesDir = coCoviseConfig::getEntry("enginesDir", "COVER.Plugin.JSBSim.Model", "D:/src/gitbase/jsbsim/aircraft/paraglider/Engines");
    std::string SystemsDir = coCoviseConfig::getEntry("systemsDir", "COVER.Plugin.JSBSim.Model", "D:/src/gitbase/jsbsim/aircraft/paraglider/Systems");
    std::string resetFile = coCoviseConfig::getEntry("resetFile", "COVER.Plugin.JSBSim.Model", "D:/src/gitbase/jsbsim/aircraft/paraglider/reset00.xml");
    // *** OPTION A: LOAD A SCRIPT, WHICH LOADS EVERYTHING ELSE *** //
    if (!ScriptName.isNull()) {

        result = FDMExec->LoadScript(ScriptName, 0.008333, SGPath(resetFile));

        if (!result) {
            cerr << "Script file " << ScriptName << " was not successfully loaded" << endl;
            return false;
        }

        // *** OPTION B: LOAD AN AIRCRAFT AND A SET OF INITIAL CONDITIONS *** //
    }
    else if (!AircraftName.empty() || !resetFile.length()==0) {

        if (catalog) FDMExec->SetDebugLevel(0);

        if (!FDMExec->LoadModel(SGPath(AircraftDir),
                                SGPath(EnginesDir),
                                SGPath(SystemsDir),
                                AircraftName)) {
            cerr << "  JSBSim could not be started" << endl << endl;
            return false;
        }

        JSBSim::FGInitialCondition *IC = FDMExec->GetIC();
        if (!IC->Load(SGPath(resetFile))) {
            cerr << "Initialization unsuccessful" << endl;
            return false;
        }
    }
    else {
        cout << "  No Aircraft, Script, or Reset information given" << endl << endl;
        return false;
    }

    frame_duration = FDMExec->GetDeltaT();

    initial_seconds = cover->frameTime();
    Propagate->InitModel();

    FDMExec->RunIC();
    FDMExec->Setsim_time(0.0);//cover->frameTime());
    osg::Vec3 viewerPosInFeet = cover->getInvBaseMat().getTrans() / 0.3048;
    JSBSim::FGColumnVector3 v(-viewerPosInFeet[1], viewerPosInFeet[0], viewerPosInFeet[2]);
    JSBSim::FGLocation l(v);
    //JSBSim::FGPropagate::VehicleState o;
    //o. //(48.678993, 8.359287, 20926069);
    fprintf(stderr,"v: %f %f %f\n",-viewerPosInFeet[1], viewerPosInFeet[0], viewerPosInFeet[2]);
    //FDMExec->GetPropagate()->SetLocation(o);

    result = FDMExec->Run();  // MAKE AN INITIAL RUN
    initialLocation = Propagate->GetVState();
    initialLocation.vLocation += JSBSim::FGLocation(v);
    FDMExec->GetPropagate()->SetLocation(initialLocation.vLocation);
    fprintf(stderr,"v: %f %f %f\n",initialLocation.vLocation(1),initialLocation.vLocation(2),initialLocation.vLocation(3));
    zeroPosition = osg::Vec3(initialLocation.vLocation(1),initialLocation.vLocation(2),initialLocation.vLocation(3));

    // PRINT SIMULATION CONFIGURATION
    FDMExec->PrintSimulationConfiguration();
    FDMExec->GetPropagate()->DumpState();
    return true;
}

bool
JSBSimPlugin::update()
{
    updateUdp();

    FCS->SetDaCmd(0.0);
    FCS->SetDeCmd(0.0);

    for (int i = 0; i < Propulsion->GetNumEngines(); i++) {
        FCS->SetThrottleCmd(i,1.0);
        FCS->SetMixtureCmd(i,1.0);

        switch (Propulsion->GetEngine(i)->GetType())
        {
        case JSBSim::FGEngine::etPiston:
        { // FGPiston code block
            JSBSim::FGPiston* eng = (JSBSim::FGPiston*)Propulsion->GetEngine(i);
            eng->SetMagnetos(3);
            break;
        } // end FGPiston code block
        }
        { // FGEngine code block
            JSBSim::FGEngine* eng = Propulsion->GetEngine(i);
            eng->SetStarter(1);
            eng->SetRunning(1);
        } // end FGEngine code block
    }
    bool result = false;

    current_seconds = cover->frameTime();
    double sim_lag_time = current_seconds - FDMExec->GetSimTime(); // How far behind sim-time is from actual
    // elapsed time.
    //for (int i = 0; i<(int)(sim_lag_time / frame_duration); i++) {  // catch up sim time to actual elapsed time.
    result = FDMExec->Run();
    //    if (FDMExec->Holding()) break;
    //}

    osg::Vec3d newPos;
    JSBSim::FGPropagate::VehicleState location = Propagate->GetVState();
    osg::Vec3d currentPosition(location.vLocation(1),location.vLocation(2),location.vLocation(3));
    newPos = currentPosition;//-zeroPosition;
    //osg::Matrix planeOrientationMatrix;
    //JSBSim::FGQuaternion currentOrientation = Propagate->GetTec2l();
    //osg::Quat currentOrientation2(currentOrientation.Entry(0),currentOrientation.Entry(1),currentOrientation.Entry(2),currentOrientation.Entry(3));
    //planeOrientationMatrix.makeRotate(currentOrientation2);
    osg::Matrix planeTranslation;
    planeTranslation.makeTranslate(newPos[2],newPos[1],newPos[0]);
    //trans.makeTranslate(location.vLocation.GetLatitude(),location.vLocation.GetLongitude(), location.vLocation.GetAltitudeASL());
    //3502274.250000 -1281103.125000 217903.953125
    if (cover->frameTime() > printTime + 5)
    {
        printTime = cover->frameTime();
        // Dump the simulation state (position, orientation, etc.)
        FDMExec->GetPropagate()->DumpState();
        //fprintf(stderr,"EPA: %f\n",Propagate->GetEarthPositionAngle());
        //fprintf(stderr,"cover->getScale(): %f\n", cover->getScale());
        //fprintf(stderr,"currentLocationOffset: %f %f %f\n",location.vLocation(1)-initialLocation.vLocation(1),location.vLocation(2)-initialLocation.vLocation(2), location.vLocation(3)-initialLocation.vLocation(3));
    }

    osg::Matrix rot;
    rot.makeRotate(Propagate->GetEuler(JSBSim::FGJSBBase::ePsi), osg::Vec3(0, 0, 1), Propagate->GetEuler(JSBSim::FGJSBBase::eTht), osg::Vec3(1, 0, 0), Propagate->GetEuler(JSBSim::FGJSBBase::ePhi), osg::Vec3(0, 1, 0));
    osg::Matrix trans;
    JSBSim::FGColumnVector3 pos = Propagate->GetInertialPosition();
    trans.makeTranslate(-pos(2)*0.3048, pos(1)*0.3048, pos(3)*0.3048);
    osg::Matrix Plane = osg::Matrix::inverse(rot* planeTranslation);
    /*JSBSim::FGMatrix33 tb2lMat = Propagate->GetTb2l();
    Plane.makeIdentity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            Plane(i, j) = tb2lMat.Entry(i+1, j+1);
     Plane.invert(Plane); */

    if (FDMExec && !FDMExec->Holding())
    {
        VRSceneGraph::instance()->getTransform()->setMatrix(Plane);
        coVRCollaboration::instance()->SyncXform();
    }
    return true;
}

bool
JSBSimPlugin::updateUdp()
{
    OpenThreads::ScopedLock<OpenThreads::Mutex> lock(mutex);
    if (udp)
    {
        int status = udp->receive(&fgcontrol, sizeof(FGControl));

        if (status == sizeof(FGControl))
        {
            for (unsigned i = 0; i < 3; ++i)
                byteSwap(fgcontrol);
            for (unsigned i = 0; i < 3; ++i)
                byteSwap(fgdata.orientation[i]);
            FCS->SetDaCmd(fgcontrol.aileron);
            FCS->SetDeCmd(fgcontrol.elevator);
        }
        else if (status == -1)
        {
            std::cerr << "FlightGear::update: error while reading data" << std::endl;
            init();
            return;
        }
        else
        {
            std::cerr << "FlightGear::update: received invalid no. of bytes: recv=" << status << ", got=" << status << std::endl;
            init();
            return;
        }
    }
}

void JSBSim::init()
{
    delete udp;

    const std::string host = covise::coCoviseConfig::getEntry("value", "JSBSim.serverHost", "141.58.8.212");
    unsigned short serverPort = covise::coCoviseConfig::getInt("JSBSim.serverPort", 1234);
    unsigned short localPort = covise::coCoviseConfig::getInt("JSBSim.localPort", 5252);
    std::cerr << "JSBSim config: UDP: serverHost: " << host << ", localPort: " << localPort << ", serverPort: " << serverPort << std::endl;
    udp = new UDPComm(host.c_str(), serverPort, localPort);
    return;
}

COVERPLUGIN(JSBSimPlugin)
