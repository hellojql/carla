// Copyright (c) 2019 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "Carla.h"

#include "Carla/Sensor/InertialMeasurementUnit.h"
#include "Carla/Actor/ActorBlueprintFunctionLibrary.h"
#include "Carla/Sensor/WorldObserver.h"
#include "Carla/Vehicle/CarlaWheeledVehicle.h"

#include <compiler/disable-ue4-macros.h>
#include "carla/geom/Math.h"
#include <compiler/enable-ue4-macros.h>

#include <limits>

// Based on OpenDRIVE's lon and lat
const FVector AInertialMeasurementUnit::CarlaNorthVector =
    FVector(0.0f, -1.0f, 0.0f);

AInertialMeasurementUnit::AInertialMeasurementUnit(
    const FObjectInitializer &ObjectInitializer)
  : Super(ObjectInitializer)
{
  PrimaryActorTick.bCanEverTick = true;
  PrimaryActorTick.TickGroup = TG_PostPhysics;
  RandomEngine = CreateDefaultSubobject<URandomEngine>(TEXT("RandomEngine"));
  PrevLocation = { FVector::ZeroVector, FVector::ZeroVector };
  // Initialized to something hight to minimize the artifacts
  // when the initial values are unknown
  PrevDeltaTime = std::numeric_limits<float>::max();
  OldTransform = GetActorTransform();
}

FActorDefinition AInertialMeasurementUnit::GetSensorDefinition()
{
  return UActorBlueprintFunctionLibrary::MakeIMUDefinition();
}

const FTransform &AInertialMeasurementUnit::GetSyncActorTransform() const {
  return OldTransform;
}

void AInertialMeasurementUnit::Set(const FActorDescription &ActorDescription)
{
  Super::Set(ActorDescription);
  UActorBlueprintFunctionLibrary::SetIMU(ActorDescription, this);
}

void AInertialMeasurementUnit::SetOwner(AActor *Owner)
{
  Super::SetOwner(Owner);
}

// Returns the angular velocity of Actor, expressed in the frame of Actor
static FVector FIMU_GetActorAngularVelocityInRadians(
    AActor &Actor)
{
  const auto RootComponent = Cast<UPrimitiveComponent>(Actor.GetRootComponent());

  FVector AngularVelocity;

  if (RootComponent != nullptr) {
      const FQuat ActorGlobalRotation = RootComponent->GetComponentTransform().GetRotation();
      const FVector GlobalAngularVelocity = RootComponent->GetPhysicsAngularVelocityInRadians();
      AngularVelocity = ActorGlobalRotation.UnrotateVector(GlobalAngularVelocity);
  } else {
      AngularVelocity = FVector::ZeroVector;
  }

  return AngularVelocity;
}

const carla::geom::Vector3D AInertialMeasurementUnit::ComputeAccelerometerNoise(
    const FVector &Accelerometer)
{
  // Normal (or Gaussian or Gauss) distribution will be used as noise function.
  // A mean of 0.0 is used as a first parameter, the standard deviation is
  // determined by the client
  constexpr float Mean = 0.0f;
  return carla::geom::Vector3D {
      Accelerometer.X + RandomEngine->GetNormalDistribution(Mean, StdDevAccel.X),
      Accelerometer.Y + RandomEngine->GetNormalDistribution(Mean, StdDevAccel.Y),
      Accelerometer.Z + RandomEngine->GetNormalDistribution(Mean, StdDevAccel.Z)
  };
}

const carla::geom::Vector3D AInertialMeasurementUnit::ComputeGyroscopeNoise(
    const FVector &Gyroscope)
{
  // Normal (or Gaussian or Gauss) distribution and a bias will be used as
  // noise function.
  // A mean of 0.0 is used as a first parameter.The standard deviation and the
  // bias are determined by the client
  constexpr float Mean = 0.0f;
  return carla::geom::Vector3D {
      Gyroscope.X + BiasGyro.X + RandomEngine->GetNormalDistribution(Mean, StdDevGyro.X),
      Gyroscope.Y + BiasGyro.Y + RandomEngine->GetNormalDistribution(Mean, StdDevGyro.Y),
      Gyroscope.Z + BiasGyro.Z + RandomEngine->GetNormalDistribution(Mean, StdDevGyro.Z)
  };
}

carla::geom::Vector3D AInertialMeasurementUnit::ComputeAccelerometer(
    const float DeltaTime)
{
  // Used to convert from UE4's cm to meters
  constexpr float TO_METERS = 1e-2;
  // Earth's gravitational acceleration is approximately 9.81 m/s^2
  constexpr float GRAVITY = 9.81f;

  // 2nd derivative of the polynomic (quadratic) interpolation
  // using the point in current time and two previous steps:
  // d2[i] = -2.0*(y1/(h1*h2)-y2/((h2+h1)*h2)-y0/(h1*(h2+h1)))
  const FVector CurrentLocation = GetActorLocation();

  const FVector Y2 = PrevLocation[0];
  const FVector Y1 = PrevLocation[1];
  const FVector Y0 = CurrentLocation;
  const float H1 = DeltaTime;
  const float H2 = PrevDeltaTime;

  const float H1AndH2 = H2 + H1;
  const FVector A = Y1 / ( H1 * H2 );
  const FVector B = Y2 / ( H2 * (H1AndH2) );
  const FVector C = Y0 / ( H1 * (H1AndH2) );
  FVector FVectorAccelerometer = TO_METERS * -2.0f * ( A - B - C );

  // Update the previous locations
  PrevLocation[0] = PrevLocation[1];
  PrevLocation[1] = CurrentLocation;
  PrevDeltaTime = DeltaTime;

  // Add gravitational acceleration
  FVectorAccelerometer.Z += GRAVITY;

  FQuat ImuRotation =
      GetRootComponent()->GetComponentTransform().GetRotation();
  FVectorAccelerometer = ImuRotation.UnrotateVector(FVectorAccelerometer);

  // Cast from FVector to our Vector3D to correctly send the data in m/s^2
  // and apply the desired noise function, in this case a normal distribution
  const carla::geom::Vector3D Accelerometer =
      ComputeAccelerometerNoise(FVectorAccelerometer);

  return Accelerometer;
}

carla::geom::Vector3D AInertialMeasurementUnit::ComputeGyroscope()
{
  const FVector AngularVelocity =
      FIMU_GetActorAngularVelocityInRadians(*GetOwner());

  const FQuat SensorLocalRotation =
      RootComponent->GetRelativeTransform().GetRotation();

  const FVector FVectorGyroscope =
      SensorLocalRotation.RotateVector(AngularVelocity);

  // Cast from FVector to our Vector3D to correctly send the data in rad/s
  // and apply the desired noise function, in this case a normal distribution
  const carla::geom::Vector3D Gyroscope =
      ComputeGyroscopeNoise(FVectorGyroscope);

  return Gyroscope;
}

float AInertialMeasurementUnit::ComputeCompass()
{
  // Magnetometer: orientation with respect to the North in rad
  const FVector ForwVect = GetActorForwardVector().GetSafeNormal2D();
  float Compass = std::acos(FVector::DotProduct(CarlaNorthVector, ForwVect));

  // Keep the angle between [0, 2pi)
  if (FVector::CrossProduct(CarlaNorthVector, ForwVect).Z < 0.0f)
  {
    Compass = carla::geom::Math::Pi2<float>() - Compass;
  }

  return Compass;
}

void AInertialMeasurementUnit::Tick(float DeltaTime)
{
  Super::Tick(DeltaTime);

  auto Stream = GetDataStream(*this);
  Stream.Send(
      *this,
      ComputeAccelerometer(DeltaTime),
      ComputeGyroscope(),
      ComputeCompass());

  // TODO: delete once the new tick pipeline is done
  OldTransform = GetActorTransform();
}

void AInertialMeasurementUnit::SetAccelerationStandardDeviation(const FVector &Vec)
{
  StdDevAccel = Vec;
}

void AInertialMeasurementUnit::SetGyroscopeStandardDeviation(const FVector &Vec)
{
  StdDevGyro = Vec;
}

void AInertialMeasurementUnit::SetGyroscopeBias(const FVector &Vec)
{
  BiasGyro = Vec;
}

const FVector &AInertialMeasurementUnit::GetAccelerationStandardDeviation() const
{
  return StdDevAccel;
}

const FVector &AInertialMeasurementUnit::GetGyroscopeStandardDeviation() const
{
  return StdDevGyro;
}

const FVector &AInertialMeasurementUnit::GetGyroscopeBias() const
{
  return BiasGyro;
}

void AInertialMeasurementUnit::BeginPlay()
{
  Super::BeginPlay();
}
